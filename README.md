# CYD Hub v6

Dashboard touchscreen per **ESP32-2432S028R (Cheap Yellow Display / CYD)** sviluppata con **PlatformIO + Arduino Framework**.

Il progetto è pensato per funzionare sia su **hardware reale** sia in **simulazione con Wokwi**, usando due environment PlatformIO separati ma lo stesso codice sorgente.

## Funzionalità

CYD Hub include diverse pagine navigabili tramite swipe orizzontale o barra di navigazione inferiore:

- **Playlist Spotify**
  - visualizzazione delle playlist dell'account
  - griglia 2×3, fino a 6 playlist per pagina
  - cambio pagina interno
  - refresh manuale
  - avvio diretto di una playlist

- **Spotify Player**
  - brano in riproduzione
  - artista
  - copertina album
  - barra di avanzamento
  - play / pausa
  - traccia precedente / successiva

- **Meteo**
  - temperatura attuale
  - temperatura percepita
  - umidità
  - vento
  - minima / massima giornaliera
  - previsioni dei giorni successivi
  - dati forniti da Open-Meteo, senza API key

- **Home / Orologio**
  - orologio digitale a grandi cifre
  - data corrente
  - riepilogo meteo compatto

- **Radar pioggia**
  - immagini radar RainViewer
  - aggiornamento periodico

- **Formula 1**
  - informazioni F1 tramite API Jolpica / Ergast
  - aggiornamento automatico

- **Calendario mensile**
  - mese corrente
  - giorno corrente evidenziato
  - navigazione tra i mesi

## Hardware supportato

Hardware principale:

```text
ESP32-2432S028R
"Cheap Yellow Display"
320×240
ILI9341 / compatibile
Touch resistivo XPT2046
```

Pin touch utilizzati:

```text
CS   = 33
IRQ  = 36
CLK  = 25
MISO = 39
MOSI = 32
```

## Ambiente di sviluppo

Il progetto utilizza:

- Visual Studio Code
- PlatformIO
- Arduino Framework
- ESP32 platform
- TFT_eSPI
- XPT2046_Touchscreen
- ArduinoJson
- JPEGDEC
- PNGdec

## Struttura del progetto

```text
CYD-Hub/
│
├── src/
│   ├── main.cpp
│   ├── config.h
│   ├── secrets.h
│   └── secrets.example.h
│
├── platformio.ini
├── diagram.json
├── wokwi.toml
├── .gitignore
└── README.md
```

`secrets.h` contiene le credenziali personali e **non deve essere pubblicato nel repository**.

## Configurazione delle credenziali

Copia:

```text
src/secrets.example.h
```

in:

```text
src/secrets.h
```

e inserisci le tue credenziali:

```cpp
#pragma once

#define REAL_WIFI_SSID "YOUR_WIFI_SSID"
#define REAL_WIFI_PASS "YOUR_WIFI_PASSWORD"

#define SPOTIFY_CLIENT_ID_VALUE "YOUR_SPOTIFY_CLIENT_ID"
#define SPOTIFY_CLIENT_SECRET_VALUE "YOUR_SPOTIFY_CLIENT_SECRET"
```

Assicurati che `.gitignore` contenga:

```gitignore
src/secrets.h
```

In questo modo il file con le credenziali resta solo sul PC locale.

## Configurazione generale

Le impostazioni non sensibili sono in:

```text
src/config.h
```

Esempio:

```cpp
#define WEATHER_CITY "Imola"
#define WEATHER_LAT "44.3592"
#define WEATHER_LON "11.7132"

#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

#define ROTATION 3
```

Per l'hardware reale vengono usate le credenziali presenti in `secrets.h`.

Per Wokwi viene invece utilizzata automaticamente la rete virtuale:

```cpp
Wokwi-GUEST
```

senza password.

## Spotify

Per utilizzare Spotify devi creare una app da:

https://developer.spotify.com/dashboard

Configura come Redirect URI:

```text
http://127.0.0.1:8888/callback
```

Il firmware utilizza gli scope necessari per:

```text
user-read-playback-state
user-modify-playback-state
user-read-currently-playing
playlist-read-private
playlist-read-collaborative
```

Al primo avvio sul CYD reale, se non è ancora presente un refresh token, il dispositivo entra nella procedura di associazione Spotify.

Il refresh token viene salvato nella memoria Preferences dell'ESP32.

Per dimenticare l'account Spotify è possibile tenere premuto il touchscreen durante l'avvio.

> Nota: alcune funzioni di controllo playback richiedono Spotify Premium e un dispositivo Spotify attivo.

## Meteo

Il progetto utilizza:

```text
Open-Meteo
```

Non è necessaria alcuna API key.

La posizione è configurata in `config.h` tramite:

```cpp
#define WEATHER_CITY "Imola"
#define WEATHER_LAT "44.3592"
#define WEATHER_LON "11.7132"
```

L'aggiornamento predefinito è:

```cpp
#define WEATHER_REFRESH_MS (15UL * 60UL * 1000UL)
```

ovvero ogni 15 minuti.

## Navigazione

Ordine delle pagine:

```text
Playlist
   ←
Spotify Player
   ←
Meteo
   ←
HOME
   →
Radar
   →
Formula 1
   →
Calendario
```

La Home è la pagina centrale.

È possibile cambiare pagina tramite:

- swipe orizzontale sul touchscreen
- pallini di navigazione nella parte inferiore dello schermo

## Build per CYD reale

Environment PlatformIO:

```text
cyd
```

Build:

```bash
pio run -e cyd
```

Upload:

```bash
pio run -e cyd -t upload
```

Il firmware compilato viene generato in:

```text
.pio/build/cyd/firmware.bin
```

## Simulazione con Wokwi

Il progetto è già configurato per essere eseguito anche nel simulatore **Wokwi**.

Environment PlatformIO:

```text
cyd-wokwi
```

Build:

```bash
pio run -e cyd-wokwi
```

Il firmware simulato viene generato in:

```text
.pio/build/cyd-wokwi/firmware.bin
```

Il file:

```text
wokwi.toml
```

punta automaticamente a:

```toml
[wokwi]
version = 1
firmware = ".pio/build/cyd-wokwi/firmware.bin"
elf = ".pio/build/cyd-wokwi/firmware.elf"
```

La board simulata è definita in:

```text
diagram.json
```

ed utilizza:

```json
{
  "type": "board-esp32-2432s028r"
}
```

### Differenze automatiche tra hardware reale e Wokwi

Quando viene compilato l'environment:

```text
cyd-wokwi
```

PlatformIO definisce:

```cpp
WOKWI
```

Il codice può quindi applicare automaticamente configurazioni differenti.

In simulazione:

- WiFi: `Wokwi-GUEST`
- password WiFi vuota
- canale WiFi 6
- autenticazione Spotify iniziale disabilitata
- inversione colori display disabilitata

Sul CYD reale:

- vengono usate le credenziali presenti in `secrets.h`
- il canale WiFi viene cercato automaticamente
- autenticazione Spotify abilitata
- configurazione display reale

## Avviare Wokwi da Visual Studio Code

Installa l'estensione Wokwi per Visual Studio Code.

Poi:

1. compila l'environment:

```bash
pio run -e cyd-wokwi
```

2. avvia la simulazione Wokwi

3. dopo una nuova build usa **Restart Simulation** per caricare il firmware aggiornato

Non utilizzare `Upload` per Wokwi.

`Upload` serve solo per scrivere il firmware sul CYD fisico.

## Build di entrambi gli environment

Per compilare entrambe le versioni:

```bash
pio run -e cyd -e cyd-wokwi
```

In alternativa puoi configurare:

```ini
[platformio]
default_envs = cyd, cyd-wokwi
```

e poi eseguire:

```bash
pio run
```

## platformio.ini

Il progetto contiene environment separati.

Esempio:

```ini
[platformio]
default_envs = cyd

[env]
platform = espressif32@^6.9.0
board = esp32dev
framework = arduino
monitor_speed = 115200

[env:cyd]
; configurazione hardware reale

[env:cyd-wokwi]
extends = env:cyd
build_flags =
    ${env:cyd.build_flags}
    -DWOKWI
```

L'environment predefinito resta `cyd`, in modo che il normale pulsante Build di PlatformIO compili il firmware destinato all'hardware reale.

## Aggiornamenti periodici

Gli intervalli principali sono configurabili in `config.h`.

Valori predefiniti:

```cpp
#define SPOTIFY_POLL_MS 4000UL
#define WEATHER_REFRESH_MS (15UL * 60UL * 1000UL)
#define RADAR_REFRESH_MS (10UL * 60UL * 1000UL)
#define F1_REFRESH_MS (6UL * 60UL * 60UL * 1000UL)
```

## Interfaccia grafica

La UI utilizza una palette scura chiamata internamente **Midnight Glass**.

Colori principali:

- fondo blu / nero
- viola e rosa per Home e Calendario
- verde per Spotify
- ciano per Meteo
- blu per Radar
- rosso per Formula 1

L'interfaccia è progettata specificamente per il display CYD da 320×240 pixel.

## Boot

All'avvio viene mostrata una schermata grafica dedicata invece di semplici messaggi testuali.

Durante l'inizializzazione vengono gestiti:

```text
WiFi
NTP / ora
Meteo
Spotify
```

La logica può essere resa ulteriormente asincrona per mostrare la Home ancora prima e completare gli aggiornamenti di rete in background.

## Wokwi e servizi Internet

Wokwi è principalmente utilizzato per:

- test grafici
- layout
- navigazione
- sviluppo senza collegare continuamente il CYD fisico

Alcune funzionalità che dipendono da autenticazioni esterne possono essere volutamente disabilitate o limitate durante la simulazione.

In particolare la procedura OAuth Spotify viene saltata nell'environment `cyd-wokwi`, così il simulatore non rimane bloccato in attesa dell'autenticazione.

## Sicurezza

Non pubblicare mai:

```text
password WiFi
Spotify Client Secret
token Spotify
refresh token
```

Il repository deve contenere solo:

```text
secrets.example.h
```

e non:

```text
secrets.h
```

Se una credenziale è stata accidentalmente pubblicata su Git, rimuoverla dal file non è sufficiente: deve essere considerata compromessa e rigenerata.

## Pulizia della build

In caso di errori strani di linker o file `.pio` corrotti:

Windows PowerShell:

```powershell
Remove-Item -Recurse -Force .pio
pio run -e cyd -j 1
```

Per Wokwi:

```powershell
Remove-Item -Recurse -Force .pio
pio run -e cyd-wokwi -j 1
```

## Stato del progetto

Il progetto è in sviluppo attivo.

Attualmente include:

```text
[✓] Home / Clock
[✓] Meteo
[✓] Spotify Player
[✓] Playlist Spotify
[✓] Radar pioggia
[✓] Formula 1
[✓] Calendario
[✓] Navigazione touchscreen
[✓] Build CYD reale
[✓] Build Wokwi
[✓] Configurazione separata delle credenziali
```

## Licenza

Aggiungi qui la licenza che vuoi utilizzare per il repository.

Per un progetto open source semplice puoi ad esempio utilizzare la licenza MIT.

---

**CYD Hub v6**

ESP32-2432S028R dashboard for home, weather, music and more.
