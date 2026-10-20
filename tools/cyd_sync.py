"""Collega il PC al CYD Hub via Bluetooth, sullo stesso abbinamento della tastiera "CYD Hub":
- sincronizza il muto del microfono predefinito di Windows (tocco su MIC = muto di sistema);
- inoltra le notifiche di Teams, Webex e Slack.

Uso:  pip install pycaw winrt-runtime winrt-Windows.Devices.Bluetooth ^
          winrt-Windows.Devices.Bluetooth.GenericAttributeProfile winrt-Windows.Devices.Enumeration ^
          winrt-Windows.Storage.Streams winrt-Windows.UI.Notifications winrt-Windows.UI.Notifications.Management ^
          winrt-Windows.Foundation winrt-Windows.Foundation.Collections winrt-Windows.ApplicationModel
      pythonw cyd_sync.py      (senza finestra; nessun indirizzo da configurare)

Per le notifiche serve l'accesso: Impostazioni > Privacy e sicurezza > Notifiche.
"""
import asyncio
import datetime
import sys
import time
import uuid
import winreg
from ctypes import POINTER, cast

sys.coinit_flags = 0  # COM multithread: in STA (default di comtypes) le chiamate Bluetooth WinRT non terminano mai
from comtypes import CLSCTX_ALL
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume
from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattClientCharacteristicConfigurationDescriptorValue, GattCommunicationStatus)
from winrt.windows.storage.streams import DataWriter
from winrt.windows.ui.notifications import KnownNotificationBindings, NotificationKinds
from winrt.windows.ui.notifications.management import UserNotificationListener, UserNotificationListenerAccessStatus

# Devono coincidere con SYNC_*_UUID in src/main.cpp.
SVC = uuid.UUID("c7d10000-5f6e-4b8a-9a2d-3c1e0f6b7a90")
MIC = uuid.UUID("c7d10001-5f6e-4b8a-9a2d-3c1e0f6b7a90")
NOTIF = uuid.UUID("c7d10002-5f6e-4b8a-9a2d-3c1e0f6b7a90")
TIMEOUT = 15  # s: oltre, una chiamata Bluetooth appesa fa ripartire il collegamento
APPS = ("teams", "webex", "slack")
OK = GattCommunicationStatus.SUCCESS


def mic():
    # Riletto a ogni giro: segue il cambio del microfono predefinito.
    dev = AudioUtilities.GetMicrophone()
    vol = getattr(dev, "EndpointVolume", None)  # pycaw recenti
    return vol or cast(dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None), POINTER(IAudioEndpointVolume))


def paired_addresses(name="CYD Hub"):
    """Indirizzi Bluetooth abbinati con quel nome, letti dal registro di Windows."""
    # La ricerca WinRT (DeviceInformation.find_all) a volte resta appesa per sempre: il registro risponde subito.
    root = r"SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices"
    found = []
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, root) as k:
        for i in range(winreg.QueryInfoKey(k)[0]):
            addr = winreg.EnumKey(k, i)
            try:
                with winreg.OpenKey(k, addr) as d:
                    if winreg.QueryValueEx(d, "Name")[0].rstrip(b"\0").decode(errors="ignore") == name:
                        found.append(int(addr, 16))
            except OSError:
                pass  # dispositivo senza nome
    return found


async def connect():
    """Apre il CYD abbinato e ritorna (device, car. microfono, car. notifiche)."""
    addrs = paired_addresses()
    if not addrs:
        raise ConnectionError("CYD Hub non abbinato al PC")
    for addr in addrs:  # piu' voci se e' stato riabbinato: la prima raggiungibile
        dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
        if dev:
            break
    else:
        raise ConnectionError("CYD Hub non trovato")
    # Prima la cache: la lettura UNCACHED fallisce spesso ("Il dispositivo non riconosce il comando").
    # Si rilegge dal CYD solo se in cache manca il servizio (es. dopo un aggiornamento del firmware).
    svcs = await dev.get_gatt_services_for_uuid_with_cache_mode_async(SVC, BluetoothCacheMode.CACHED)
    if svcs.status != OK or not svcs.services:
        svcs = await dev.get_gatt_services_for_uuid_with_cache_mode_async(SVC, BluetoothCacheMode.UNCACHED)
    if svcs.status != OK or not svcs.services:
        raise ConnectionError("servizio non trovato (CYD spento, Bluetooth disattivato o firmware vecchio)")
    svc = svcs.services[0]
    chars = [(await svc.get_characteristics_for_uuid_async(u)).characteristics[0] for u in (MIC, NOTIF)]
    return dev, *chars


async def write(ch, data: bytes):
    w = DataWriter()
    w.write_bytes(data)
    if (await asyncio.wait_for(ch.write_value_with_result_async(w.detach_buffer()), TIMEOUT)).status != OK:
        raise ConnectionError("scrittura Bluetooth fallita")


def notif_payload(n, start):
    """Campi separati da 0x1F: id, old, ora, app, titolo, testo. None se l'app non e' seguita."""
    app = n.app_info.display_info.display_name
    if not any(a in app.lower() for a in APPS):
        return None
    b = n.notification.visual.get_binding(KnownNotificationBindings.toast_generic)
    texts = [t.text for t in b.get_text_elements()] if b else []
    fields = [str(n.id), "1" if n.creation_time < start else "0",
              n.creation_time.astimezone().strftime("%H:%M"), app[:24],
              texts[0][:60] if texts else "", " ".join(texts[1:])[:100]]
    # Sta in un solo pacchetto BLE; il taglio puo' spezzare un carattere UTF-8, che viene scartato.
    return "\x1f".join(f.replace("\x1f", " ") for f in fields).encode()[:240].decode(errors="ignore").encode()


async def session(listener):
    dev, mic_ch, notif_ch = await asyncio.wait_for(connect(), TIMEOUT)
    # A ogni collegamento si reinvia tutto: le notifiche gia' presenti vanno nello storico senza popup.
    start = datetime.datetime.now(datetime.timezone.utc)
    sent = set()
    loop = asyncio.get_running_loop()
    toggle = asyncio.Event()
    mic_ch.add_value_changed(lambda c, a: loop.call_soon_threadsafe(toggle.set))  # 't' = tocco su MIC
    status = await asyncio.wait_for(mic_ch.write_client_characteristic_configuration_descriptor_async(
        GattClientCharacteristicConfigurationDescriptorValue.NOTIFY), TIMEOUT)
    if status != OK:
        raise ConnectionError("iscrizione alle notifiche del CYD fallita")
    print("Collegato al CYD Hub")
    last_mute, last_beat, next_notif = None, 0.0, 0.0
    while True:
        vol = mic()
        if toggle.is_set():
            toggle.clear()
            vol.SetMute(not vol.GetMute(), None)
        muted = bool(vol.GetMute())
        # Anche se non cambia, lo stato si riscrive ogni secondo: il CYD lo usa per sapere che lo script e' attivo.
        if muted != last_mute or time.monotonic() - last_beat > 1:
            await write(mic_ch, b"1" if muted else b"0")
            last_mute, last_beat = muted, time.monotonic()
        if listener and time.monotonic() >= next_notif:
            next_notif = time.monotonic() + 1
            notifs = await asyncio.wait_for(listener.get_notifications_async(NotificationKinds.TOAST), TIMEOUT)
            for n in sorted(notifs, key=lambda n: n.creation_time):
                if n.id in sent:
                    continue
                try:
                    payload = notif_payload(n, start)
                except Exception as e:
                    print("Notifica ignorata:", e)
                    payload = None
                if payload:
                    await write(notif_ch, payload)
                sent.add(n.id)
            sent &= {n.id for n in notifs}  # dimentica le notifiche chiuse sul PC
        try:
            await asyncio.wait_for(toggle.wait(), 0.2)
        except TimeoutError:
            pass


async def main():
    listener = UserNotificationListener.current
    if await listener.request_access_async() != UserNotificationListenerAccessStatus.ALLOWED:
        print("Accesso alle notifiche negato: solo sincronizzazione del microfono")
        listener = None
    while True:
        try:
            await session(listener)
        except Exception as e:
            print("CYD non raggiungibile:", repr(e))  # repr: TimeoutError ha messaggio vuoto
        await asyncio.sleep(5)


asyncio.run(main())
