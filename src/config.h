#pragma once

#ifndef WOKWI
#include "secret.h"
#endif

// ===================== WiFi / piattaforma =====================
// Wokwi usa la rete virtuale sul canale 6.
// Sul CYD reale il canale 0 lascia all'ESP32 la ricerca automatica del canale AP.
// ===================== WiFi / piattaforma =====================

#ifdef WOKWI

#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""
#define WIFI_CHANNEL 6
#define REQUIRE_SPOTIFY_AUTH 0

#else

#define WIFI_SSID REAL_WIFI_SSID
#define WIFI_PASS REAL_WIFI_PASS
#define WIFI_CHANNEL 0
#define REQUIRE_SPOTIFY_AUTH 1

#endif

// ===================== Spotify =====================
// Da https://developer.spotify.com/dashboard  (crea un'app)
// Redirect URI da registrare nell'app:  http://127.0.0.1:8888/callback
#ifdef WOKWI

// In simulazione non servono credenziali reali
#define SPOTIFY_CLIENT_ID ""
#define SPOTIFY_CLIENT_SECRET ""

#else

#define SPOTIFY_CLIENT_ID SPOTIFY_CLIENT_ID_VALUE
#define SPOTIFY_CLIENT_SECRET SPOTIFY_CLIENT_SECRET_VALUE

#endif

// ===================== Meteo (Open-Meteo, nessuna API key) =====================
#define WEATHER_CITY "Imola"
#define WEATHER_LAT "44.3592"
#define WEATHER_LON "11.7132"

// ===================== Orario =====================
// Italia: CET/CEST con ora legale automatica
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

// ===================== Display =====================
#define ROTATION 3 // 1 oppure 3 (ruota di 180 gradi)
#ifdef WOKWI
// Il CYD simulato da Wokwi non richiede l'inversione colori.
#define DISPLAY_INVERT false
#else
// Mantiene il comportamento del CYD reale attuale.
#define DISPLAY_INVERT true
#endif

// Calibrazione touch (valori tipici CYD, ritocca se il tocco e' spostato)
#define TS_MINX 200
#define TS_MAXX 3700
#define TS_MINY 240
#define TS_MAXY 3800

// ===================== Intervalli =====================
#define SPOTIFY_POLL_MS 4000UL
#define WEATHER_REFRESH_MS (15UL * 60UL * 1000UL)
#define RADAR_REFRESH_MS (10UL * 60UL * 1000UL)
#define F1_REFRESH_MS (6UL * 60UL * 60UL * 1000UL)
