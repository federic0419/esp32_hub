# CYD Hub v7

Dashboard touchscreen per **ESP32-2432S028R (Cheap Yellow Display / CYD)**, sviluppata con **PlatformIO + Arduino Framework** e interfaccia grafica scura **Ink**.

La Home è l'orologio; le altre funzioni sono distribuite su sette pagine raggiungibili tramite **swipe orizzontale** o dalla **barra di navigazione inferiore**.

## Novità della v7

La v7 consolida le modifiche introdotte nelle ultime revisioni del progetto:

- nuova pagina **SPORT** che raccoglie nello stesso schermo:
  - prossimo GP di Formula 1;
  - prossime partite della Juventus;
- calendario Juventus letto dal feed iCalendar di **Fixtur.es**;
- **Calendario mensile** mantenuto come pagina indipendente;
- sezione Spotify **Playlist / Coda** con due tab;
- **Coda Spotify scrollabile verticalmente**, senza paginazione;
- pulsante **CODA** direttamente nel player Spotify;
- barra inferiore ridisegnata anche dopo refresh o redraw completi delle pagine;
- sorgente `main.cpp` riorganizzato visivamente con separatori espliciti per ogni pagina;
- numero versione aggiornato a **v7** anche nella schermata di boot.

---

## Hardware

Target principale:

```text
ESP32-2432S028R
Cheap Yellow Display (CYD)
Display 320 x 240
Touch resistivo XPT2046
```

Pin touch utilizzati dal firmware:

```text
CS   = 33
IRQ  = 36
CLK  = 25
MISO = 39
MOSI = 32
```

Il progetto contiene configurazioni PlatformIO per:

- `cyd` - CYD standard con driver ILI9341;
- `cyd2usb` - variante CYD con driver ST7789;
- `cyd-wokwi` - simulazione Wokwi.

---


## Anteprima delle pagine

Le immagini seguenti sono **mockup tecnici 320×240** ricostruiti dal layout del `main.cpp`.
I dati mostrati sono dimostrativi; disposizione, palette e controlli riprendono la UI **Ink** della v7.

| Pagina | Anteprima |
|---|---|
| Playlist / Coda Spotify | ![Playlist e coda Spotify](docs/images/01_playlist_coda.png) |
| Spotify Player | ![Spotify Player](docs/images/02_spotify_player.png) |
| Meteo | ![Meteo](docs/images/03_meteo.png) |
| Orologio / Home | ![Orologio Home](docs/images/04_orologio_home.png) |
| Radar pioggia | ![Radar pioggia](docs/images/05_radar_pioggia.png) |
| Sport - F1 + Juventus | ![Sport F1 e Juventus](docs/images/06_sport_f1_juventus.png) |
| Calendario mensile | ![Calendario mensile](docs/images/07_calendario_mensile.png) |


## Pagine dell'interfaccia

L'ordine delle pagine è:

```text
0  Playlist / Coda Spotify
1  Spotify Player
2  Meteo
3  Orologio / HOME
4  Radar pioggia
5  Sport - Formula 1 + Juventus
6  Calendario mensile
```

La **Home** corrisponde alla pagina 3 ed è quindi al centro della navigazione.

### Pagina 1 - Playlist / Coda Spotify

La schermata Spotify Library contiene due tab.

**PLAYLIST**

- visualizzazione delle playlist dell'account;
- griglia 2 x 3;
- 6 playlist per pagina;
- navigazione tra le pagine;
- refresh manuale;
- tap su una playlist per avviarne la riproduzione.

**CODA**

- lettura della coda corrente di Spotify;
- elenco verticale dei brani;
- visualizzazione di titolo e artista;
- swipe verticale per scorrere avanti e indietro;
- scrollbar laterale che indica la posizione nella lista;
- tap su un brano per riprodurre a partire da quel punto della coda.

La coda **non è paginata**.

### Pagina 2 - Spotify Player

Mostra lo stato del player Spotify:

- copertina del brano;
- titolo;
- artista;
- stato play / pausa;
- barra di avanzamento;
- tempo trascorso e durata;
- brano precedente;
- play / pausa;
- brano successivo;
- pulsante **CODA** nella barra superiore per aprire direttamente la coda.

> Alcune operazioni di controllo del playback richiedono un account Spotify Premium e un dispositivo Spotify attivo.

### Pagina 3 - Meteo

I dati vengono letti da **Open-Meteo** e non richiedono API key.

La pagina mostra, in base ai dati disponibili:

- temperatura attuale;
- temperatura percepita;
- umidità;
- vento;
- min/max giornaliera;
- descrizione e icona meteo;
- previsione dei giorni successivi.

La località è impostata in `src/config.h` tramite latitudine e longitudine.

### Pagina 4 - Orologio / HOME

È la pagina principale del dispositivo.

Contiene:

- orologio digitale a grandi cifre;
- data corrente;
- indicazione del giorno della settimana;
- riepilogo meteo compatto;
- temperatura minima e massima.

Non è presente uno screensaver automatico: il display rimane sulla pagina selezionata.

### Pagina 5 - Radar pioggia

Il radar usa i dati pubblici di **RainViewer**.

Funzioni principali:

- recupero della mappa radar più recente;
- download delle tile necessarie;
- visualizzazione dell'area configurata;
- refresh periodico;
- refresh manuale dall'interfaccia.

### Pagina 6 - SPORT

La pagina SPORT unisce **Formula 1** e **Juventus**.

#### Formula 1

I dati vengono recuperati tramite API **Jolpica / Ergast compatible**.

La schermata mostra il prossimo GP disponibile, con informazioni come:

- round;
- nome del GP;
- data e ora locale;
- countdown all'evento;
- eventuale indicazione Sprint.

#### Juventus

Le partite vengono lette dal feed iCalendar:

```text
https://ics.fixtur.es/v2/juventus.ics
```

La pagina mostra le prossime partite disponibili con:

- data;
- ora locale;
- squadre / titolo dell'evento.

Il pulsante refresh della pagina SPORT aggiorna sia Formula 1 sia Juventus.

### Pagina 7 - Calendario mensile

Il calendario mensile è indipendente dal calendario Juventus.

Permette di:

- vedere il mese corrente;
- evidenziare il giorno corrente;
- passare al mese precedente;
- passare al mese successivo;
- tornare rapidamente al mese corrente.

---

## Navigazione touchscreen

Sono disponibili due modalità principali di navigazione:

- **swipe orizzontale** per passare da una pagina all'altra;
- **barra inferiore** per aprire direttamente una delle sette pagine.

Alcune pagine gestiscono gesture interne:

- nella **Coda Spotify**, lo swipe verticale scorre la lista;
- nel **Calendario**, i controlli laterali cambiano mese;
- nelle **Playlist Spotify**, le frecce cambiano pagina.

La barra inferiore viene ridisegnata anche dopo gli aggiornamenti completi delle pagine Spotify e SPORT, così rimane sempre disponibile.

---

## Servizi Internet utilizzati

| Funzione | Servizio | API key esterna |
|---|---|---|
| Spotify | Spotify Web API | OAuth Spotify |
| Meteo | Open-Meteo | No |
| Radar | RainViewer | No |
| Formula 1 | Jolpica / Ergast | No |
| Juventus | Fixtur.es iCalendar | No |
| Ora | NTP | No |

Il dispositivo deve avere accesso a Internet per aggiornare questi servizi.

---

## Struttura del progetto

```text
CYD-Hub/
|
|-- src/
|   |-- main.cpp
|   |-- config.h
|   |-- secret.h              # credenziali locali, NON versionare
|   `-- secret_example.h
|
|-- platformio.ini
|-- diagram.json
|-- wokwi.toml
|-- .gitignore
`-- README.md
```

`secret.h` è escluso dal repository tramite `.gitignore`.

---

## Organizzazione di `main.cpp` in v7

Per rendere più leggibile il sorgente, i blocchi principali sono identificati da separatori del tipo:

```cpp
// ########==================== PAGINA 4 - OROLOGIO / HOME ====================########
```

Le sezioni principali sono:

```text
HARDWARE / PALETTE / DISPLAY
UTILITY / UI COMUNE
SERVIZIO SPOTIFY - AUTH + API COMUNE
PAGINA 1 - PLAYLIST / CODA SPOTIFY
PAGINA 2 - SPOTIFY PLAYER
PAGINA 3 - METEO
PAGINA 4 - OROLOGIO / HOME
PAGINA 5 - RADAR PIOGGIA
PAGINA 6 - SPORT / FORMULA 1
PAGINA 6 - SPORT / JUVENTUS + UI SPORT
PAGINA 7 - CALENDARIO MENSILE
NAVIGAZIONE / GESTIONE PAGINE
TOUCH / GESTURE
BOOT ANIMATION
TEST CALIBRAZIONE COLORI
SETUP / LOOP PRINCIPALE
```

La parte SPORT è divisa internamente in due blocchi perché Formula 1 e Juventus usano sorgenti dati e parser differenti, ma costituiscono un'unica pagina dell'interfaccia.

---

## Configurazione WiFi e credenziali

Copia:

```text
src/secret_example.h
```

in:

```text
src/secret.h
```

e inserisci i dati reali.

Esempio:

```cpp
#pragma once

#define WIFI_SSID_BACKUP "..."
#define WIFI_PASS_BACKUP "..."

#define WIFI_SSID_PRIMARY "..."
#define WIFI_PASS_PRIMARY "..."

#define SPOTIFY_CLIENT_ID_VALUE "..."
#define SPOTIFY_CLIENT_SECRET_VALUE "..."
```

Il firmware usa `secret.h` solo sull'hardware reale.

Non inserire mai `secret.h` in Git.

---

## Configurazione generale

Le impostazioni non sensibili sono in:

```text
src/config.h
```

Tra queste:

```cpp
#define WEATHER_CITY "Imola"
#define WEATHER_LAT "44.3592"
#define WEATHER_LON "11.7132"

#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

#define ROTATION 3
```

La stringa `TZ_INFO` gestisce automaticamente CET/CEST per l'Italia.

---

## Spotify - configurazione OAuth

Crea un'app dalla dashboard sviluppatori Spotify e registra questo Redirect URI:

```text
http://127.0.0.1:8888/callback
```

Il firmware richiede gli scope:

```text
user-read-playback-state
user-modify-playback-state
user-read-currently-playing
playlist-read-private
playlist-read-collaborative
```

Al primo avvio su hardware reale, se non è presente un refresh token, il CYD avvia la procedura di associazione dell'account Spotify.

Il refresh token viene memorizzato tramite `Preferences` dell'ESP32.

Per dimenticare l'account Spotify è possibile tenere premuto il touchscreen durante l'avvio: il firmware cancella il refresh token salvato.

---

## Intervalli di aggiornamento

I principali intervalli sono definiti in `src/config.h`:

```cpp
#define SPOTIFY_POLL_MS 4000UL
#define WEATHER_REFRESH_MS (15UL * 60UL * 1000UL)
#define RADAR_REFRESH_MS (10UL * 60UL * 1000UL)
#define F1_REFRESH_MS (6UL * 60UL * 60UL * 1000UL)
```

Il calendario Juventus mantiene inoltre una propria logica di cache/refresh nel `main.cpp`.

---

## Build con PlatformIO

### CYD standard

```bash
pio run -e cyd
```

Upload:

```bash
pio run -e cyd -t upload
```

### CYD 2 USB / ST7789

```bash
pio run -e cyd2usb
```

Upload:

```bash
pio run -e cyd2usb -t upload
```

### Wokwi

```bash
pio run -e cyd-wokwi
```

Il progetto contiene:

```text
diagram.json
wokwi.toml
```

per la simulazione del CYD in Wokwi.

> La v7 qui documentata descrive il sorgente e la configurazione del progetto; la build Wokwi va verificata quando si modificano le macro WiFi o le differenze tra ambiente reale e simulato.

---

## Librerie principali

Configurate in `platformio.ini`:

- `TFT_eSPI`
- `XPT2046_Touchscreen`
- `ArduinoJson`
- `JPEGDEC`
- `PNGdec`

Il framework utilizzato è **Arduino** su piattaforma **Espressif32**.

---

## Boot

Durante l'avvio il display mostra la schermata **CYD HUB v7** e lo stato delle principali inizializzazioni:

```text
WiFi
NTP / sincronizzazione ora
Meteo
Spotify
Ready
```

Al termine viene aperta la pagina **HOME / Orologio**.

---

## Sicurezza

Non pubblicare mai nel repository:

```text
password WiFi
Spotify Client Secret
token Spotify
refresh token
src/secret.h
```

Il repository deve contenere soltanto un file di esempio, ad esempio:

```text
src/secret_example.h
```

Se una credenziale è già stata pubblicata su Git, rimuoverla dal file non è sufficiente: deve essere considerata compromessa e rigenerata.

---

## Versione

```text
CYD Hub v7
```

Interfaccia: **Ink**  
Target principale: **ESP32-2432S028R / CYD 320x240**
