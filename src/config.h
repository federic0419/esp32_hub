#pragma once

// Wokwi: modalita' demo OFFLINE. Non richiede credenziali reali.
#ifdef WOKWI
  #define WIFI_SSID "Wokwi-GUEST"
  #define WIFI_PASS ""
  #define WIFI_SSID_PRIMARY WIFI_SSID
  #define WIFI_PASS_PRIMARY WIFI_PASS
  #define WIFI_CHANNEL 6
  #define SPOTIFY_CLIENT_ID "wokwi-demo"
  #define SPOTIFY_CLIENT_SECRET "wokwi-demo"
  #define REQUIRE_SPOTIFY_AUTH 0
#else
  #include "secret.h"  // Solo ESP32 fisico
  #define WIFI_CHANNEL 0
  #define REQUIRE_SPOTIFY_AUTH 1
  #define WIFI_SSID_1 WIFI_SSID_PRIMARY
  #define WIFI_PASS_1 WIFI_PASS_PRIMARY
  #define WIFI_SSID_2 WIFI_SSID_BACKUP
  #define WIFI_PASS_2 WIFI_PASS_BACKUP
  #define SPOTIFY_CLIENT_ID SPOTIFY_CLIENT_ID_VALUE
  #define SPOTIFY_CLIENT_SECRET SPOTIFY_CLIENT_SECRET_VALUE
#endif

// Localita' meteo e orologio: Imola.
#define WEATHER_CITY "Imola"
#define WEATHER_LAT "44.3592"
#define WEATHER_LON "11.7132"
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

// Display CYD (320x240 in landscape)
#define ROTATION 3
#ifdef WOKWI
  #define DISPLAY_INVERT false
#else
  #define DISPLAY_INVERT true
#endif

// Touch
#define TS_MINX 200
#define TS_MAXX 3700
#define TS_MINY 240
#define TS_MAXY 3800

#define SPOTIFY_POLL_MS 4000UL
#define WEATHER_REFRESH_MS (15UL * 60UL * 1000UL)
#define RADAR_REFRESH_MS (10UL * 60UL * 1000UL)
#define F1_REFRESH_MS (6UL * 60UL * 60UL * 1000UL)
