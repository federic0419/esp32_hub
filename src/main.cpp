// CYD Hub v7: Spotify + Playlist/Coda + Orologio + Meteo + Radar + F1/Juventus + Controllo PC (BLE)
// Hardware: ESP32-2432S028R ("Cheap Yellow Display")
// Home: orologio al centro; le altre pagine sono ai lati con swipe sinistra/destra.
// Nessuno screensaver automatico: il display resta stabile sulla pagina corrente.

#include <Arduino.h>
// NimBLE prima di PNGdec: zutil.h di PNGdec definisce la macro "local" che rompe gli header BLE.
#ifndef WOKWI
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#endif
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <mutex>
#include <string>
#include "mbedtls/base64.h"
#include "config.h"

#define HUB_VERSION "v7.5" // da aumentare a ogni modifica del firmware (7.1, 7.2, ...)
#define HUB_USER_AGENT "CYD-Hub/7.0"

// ########==================== HARDWARE / PALETTE / DISPLAY ====================########
#define TOUCH_CS 33
#define TOUCH_IRQ 36
#define TOUCH_CLK 25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32

// LED RGB del CYD, attivi bassi
#define RGB_LED_R 4
#define RGB_LED_G 16
#define RGB_LED_B 17

// Il LED si puo' disattivare dal menu SET della HOME.
bool rgbLedEnabled = true;

TFT_eSPI tft;
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
JPEGDEC *jpeg = nullptr; // ~18 KB: allocato solo mentre si decodifica la copertina
PNG *png = nullptr; // ~45 KB: allocato solo durante la decodifica del radar, per lasciare RAM a TLS e copertine
Preferences prefs;

#define C565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
// Palette "Nebula": fondo scuro piatto, card in vetro con riflesso sul bordo
// superiore, accenti saturi (uno per pagina).
constexpr uint16_t C_BG = C565(8, 10, 15);
constexpr uint16_t C_NAV = C565(20, 20, 38);
constexpr uint16_t C_PANEL = C565(22, 23, 44);
constexpr uint16_t C_PANEL2 = C565(36, 37, 66);
constexpr uint16_t C_LINE = C565(56, 58, 96);
constexpr uint16_t C_HAIR = C565(34, 36, 62);
constexpr uint16_t C_FG = C565(250, 250, 255);
constexpr uint16_t C_DIM = C565(185, 190, 215);
constexpr uint16_t C_FAINT = C565(100, 104, 142);
constexpr uint16_t C_TRACK = C565(42, 44, 74);
constexpr uint16_t C_GREEN = C565(30, 230, 110);
constexpr uint16_t C_CYAN = C565(64, 210, 255);
constexpr uint16_t C_AMBER = C565(255, 255, 0);
constexpr uint16_t C_ORANGE = C565(255, 120, 40);
constexpr uint16_t C_SUN = C565(255, 230, 40);
constexpr uint16_t C_BLUE = C565(92, 142, 255);
constexpr uint16_t C_RED = C565(255, 40, 60);
constexpr uint16_t C_VIOLET = C565(172, 120, 255);
constexpr uint16_t C_PINK = C565(255, 80, 190);
constexpr uint16_t C_CLOUD = C565(220, 228, 242);
constexpr uint16_t C_DARK = C565(120, 132, 164);
constexpr uint16_t C_RAIN = C565(90, 182, 255);

// Spotify: stessa grammatica "Nebula" ma con base verde-petrolio.
constexpr uint16_t SP_BG = C565(6, 17, 22);
constexpr uint16_t SP_NAV = C565(12, 30, 34);
constexpr uint16_t SP_PANEL = C565(16, 36, 42);
constexpr uint16_t SP_PANEL2 = C565(24, 52, 58);
constexpr uint16_t SP_LINE = C565(44, 82, 86);
constexpr uint16_t SP_GREEN = C565(27, 232, 163);
constexpr uint16_t SP_MUTED = C565(150, 175, 182);
constexpr uint16_t SP_FAINT = C565(88, 124, 130);
constexpr uint16_t SP_BAR = C565(40, 66, 72);

constexpr int NAV_Y = 211; // tab bar: y 211..240
constexpr int NAV_H = 29;
int lastTopBarMinute = -1;
bool bootStatic = false;

// dichiarazione anticipata
extern int curPage;
void switchPage(int n);
void drawNavBar();
void openSpotifyQueue();
void updateRgbLed();
void setBootRgbLed(int active);
void runTouchCalibrationMenu();

// Stato del tocco corrente (gestito in handleTouch), usato anche per l'auto-repeat.
extern bool tsDown;
extern uint32_t tsStartT;
extern int sx, sy, lx, ly;

// Stato Bluetooth (sezione SERVIZIO BLUETOOTH), mostrato anche nella testata.
extern bool bleEnabled;
extern volatile bool bleConnected;
extern volatile bool bleTopDirty;

// Le pagine Spotify hanno uno sfondo proprio: la testata deve usare lo stesso colore.
bool isSpotifyPage(int page) { return page == 0 || page == 1; }

// Confronto tra timestamp millis() robusto all'overflow (~49 giorni).
inline bool timeReached(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

// ----- Sfondo -----
// Non c'e' framebuffer: ogni riga ha un colore noto (bgLut), cosi' ogni area puo'
// essere ripulita (fillBg) e ogni bordo anti-aliasing conosce il colore che ha sotto.
// Oggi lo sfondo e' piatto; la tabella permette di tornare a una sfumatura.
uint16_t bgLut[240];
uint16_t themeAccent = C_AMBER;

uint16_t mix565(uint16_t a, uint16_t b, uint8_t amount);

void applyTheme(uint16_t accent, bool spotify)
{
  themeAccent = accent;
  const uint16_t base = spotify ? SP_BG : C_BG;
  for (int y = 0; y < 240; y++)
    bgLut[y] = base;
}

inline uint16_t bgAt(int y) { return bgLut[constrain(y, 0, 239)]; }

// Ripulisce un rettangolo ridisegnando lo sfondo sfumato (righe uguali raggruppate).
void fillBg(int x, int y, int w, int h)
{
  if (x < 0)
  {
    w += x;
    x = 0;
  }
  if (y < 0)
  {
    h += y;
    y = 0;
  }
  w = min(w, 320 - x);
  h = min(h, 240 - y);
  if (w <= 0 || h <= 0)
    return;
  int runStart = y;
  for (int yy = y + 1; yy <= y + h; yy++)
  {
    if (yy == y + h || bgLut[yy] != bgLut[runStart])
    {
      tft.fillRect(x, runStart, w, yy - runStart, bgLut[runStart]);
      runStart = yy;
    }
  }
}

void drawBackdrop() { fillBg(0, 0, 320, 240); }

// ########==================== UTILITY / UI COMUNE ====================########
// I font integrati sono solo ASCII: converte accenti e caratteri UTF-8 comuni.
String fold(const char *s)
{
  static const char *L1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  String o;
  const uint8_t *p = (const uint8_t *)s;
  while (*p)
  {
    uint8_t c = *p;
    if (c < 0x80)
    {
      o += (char)c;
      p++;
    }
    else if (c == 0xC3 && p[1])
    {
      uint8_t d = p[1];
      o += (d >= 0x80 && d <= 0xBF) ? L1[d - 0x80] : '?';
      p += 2;
    }
    else if (c == 0xE2 && p[1] == 0x80 && p[2])
    {
      uint8_t e = p[2];
      if (e == 0x98 || e == 0x99)
        o += '\'';
      else if (e == 0x9C || e == 0x9D)
        o += '"';
      else if (e == 0x93 || e == 0x94)
        o += '-';
      else if (e == 0xA6)
        o += "...";
      else
        o += '?';
      p += 3;
    }
    else
    {
      int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3
                              : (c >= 0xC0)   ? 2
                                              : 1;
      for (int i = 0; i < len && *p; i++)
        p++;
      if (!o.endsWith("?"))
        o += '?';
    }
  }
  return o;
}

std::vector<String> wrapText(const String &s, int font, int maxW)
{
  std::vector<String> lines;
  String cur;
  int i = 0, n = s.length();
  while (i < n)
  {
    int j = s.indexOf(' ', i);
    if (j < 0)
      j = n;
    String w = s.substring(i, j);
    i = j + 1;
    if (!w.length())
      continue;
    String t = cur.length() ? cur + " " + w : w;
    if (tft.textWidth(t, font) <= maxW)
    {
      cur = t;
      continue;
    }
    if (cur.length())
    {
      lines.push_back(cur);
      cur = "";
    }
    while (tft.textWidth(w, font) > maxW && w.length() > 1)
    {
      int k = w.length();
      while (k > 1 && tft.textWidth(w.substring(0, k), font) > maxW)
        k--;
      lines.push_back(w.substring(0, k));
      w = w.substring(k);
    }
    cur = w;
  }
  if (cur.length())
    lines.push_back(cur);
  return lines;
}

// Disegna le righe (max maxLines, con "..." se tagliate). Ritorna la y finale.
int drawLines(std::vector<String> &lines, int x, int y, int font, int lh, int maxLines, uint16_t col, int maxW)
{
  tft.setTextColor(col);
  tft.setTextDatum(TL_DATUM);
  int n = min((int)lines.size(), maxLines);
  for (int i = 0; i < n; i++)
  {
    String l = lines[i];
    if (i == n - 1 && (int)lines.size() > maxLines)
    {
      l += "...";
      while (l.length() > 4 && tft.textWidth(l, font) > maxW)
        l.remove(l.length() - 4, 1);
    }
    tft.drawString(l, x, y, font);
    y += lh;
  }
  return y;
}

uint8_t c5to8(uint16_t c) { return ((c >> 11) & 0x1F) * 255 / 31; }
uint8_t c6to8(uint16_t c) { return ((c >> 5) & 0x3F) * 255 / 63; }
uint8_t cb5to8(uint16_t c) { return (c & 0x1F) * 255 / 31; }

uint16_t mix565(uint16_t a, uint16_t b, uint8_t amount)
{
  uint16_t ia = 255 - amount;
  uint8_t r = (c5to8(a) * ia + c5to8(b) * amount) / 255;
  uint8_t g = (c6to8(a) * ia + c6to8(b) * amount) / 255;
  uint8_t bl = (cb5to8(a) * ia + cb5to8(b) * amount) / 255;
  return C565(r, g, bl);
}

uint16_t pageAccent(int page)
{
  static const uint16_t A[] = {SP_GREEN, SP_GREEN, C_ORANGE, C_CYAN, C_AMBER, C_BLUE, C_RED, C_VIOLET, C_PINK};
  constexpr int N = sizeof(A) / sizeof(A[0]);
  return A[constrain(page, 0, N - 1)];
}

String upperStr(const char *s)
{
  String o = fold(s);
  o.toUpperCase();
  return o;
}

// Rettangolo arrotondato con bordi anti-aliasing (bg = colore sotto).
// Rettangolo arrotondato con bordi anti-aliasing (bg = colore sotto, -1 = sfondo della pagina).
void rrect(int x, int y, int w, int h, int r, uint16_t c, int32_t bg = -1)
{
  if (w <= 0 || h <= 0)
    return;
  uint16_t under = bg < 0 ? bgAt(y + h / 2) : (uint16_t)bg;
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  if (r < 1)
    tft.fillRect(x, y, w, h, c);
  else
    tft.fillSmoothRoundRect(x, y, w, h, r, c, under);
}

// Pannello piatto con bordo sottile.
// Card "vetro": bordo sottile, riempimento scuro e riflesso chiaro sul lato superiore.
void panel(int x, int y, int w, int h, int r = 10, uint16_t fill = C_PANEL, uint16_t border = C_LINE, int32_t bg = -1)
{
  uint16_t under = bg < 0 ? bgAt(y + h / 2) : (uint16_t)bg;
  rrect(x, y, w, h, r, border, under);
  rrect(x + 1, y + 1, w - 2, h - 2, max(r - 1, 0), fill, border);
  if (w > 2 * r + 4)
    tft.drawFastHLine(x + r, y + 1, w - 2 * r, mix565(fill, C_FG, 34));
}

void disc(int cx, int cy, int r, uint16_t c, int32_t bg = -1)
{
  tft.fillSmoothCircle(cx, cy, r, c, bg < 0 ? bgAt(cy) : (uint16_t)bg);
}

// Disco con alone luminoso: anelli concentrici che sfumano verso lo sfondo.
void glowDisc(int cx, int cy, int r, uint16_t c, int halo = 6, int32_t bg = -1, uint8_t strength = 120)
{
  uint16_t under = bg < 0 ? bgAt(cy) : (uint16_t)bg;
  const uint16_t base = under;
  for (int i = halo; i >= 1; i--)
  {
    uint16_t col = mix565(base, c, (uint8_t)(strength * (halo + 1 - i) / (halo + 1) / 2));
    tft.fillSmoothCircle(cx, cy, r + i, col, under);
    under = col;
  }
  tft.fillSmoothCircle(cx, cy, r, c, under);
}

void pill(int x, int y, int w, int h, const String &txt, uint16_t fg, uint16_t fill, int font = 1, int32_t under = -1)
{
  rrect(x, y, w, h, h / 2, fill, under);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, fill);
  tft.drawString(txt, x + w / 2, y + h / 2 + (font == 1 ? 1 : 0), font);
  tft.setTextDatum(TL_DATUM);
}

// Anello (es. simbolo di grado).
void ring(int cx, int cy, int r, int thick, uint16_t c, int32_t bg = -1)
{
  uint16_t under = bg < 0 ? bgAt(cy) : (uint16_t)bg;
  tft.fillSmoothCircle(cx, cy, r, c, under);
  tft.fillSmoothCircle(cx, cy, r - thick, under, c);
}

void drawWifiIcon(int x, int y, uint16_t col)
{
  int bars = 0;
  if (WiFi.status() == WL_CONNECTED)
  {
    int rssi = WiFi.RSSI();
    bars = (rssi > -58) ? 3 : (rssi > -70) ? 2
                                           : 1;
  }
  for (int i = 0; i < 3; i++)
  {
    int h = 4 + i * 3;
    rrect(x + i * 5, y + 10 - h, 3, h, 1, i < bars ? col : C_LINE);
  }
}

// Runa Bluetooth 7x11: accesa se il PC e' collegato.
void drawBtIcon(int cx, int cy, uint16_t c)
{
  uint16_t bg = bgAt(cy);
  tft.drawFastVLine(cx, cy - 5, 11, c);
  tft.drawWideLine(cx, cy - 5, cx + 3, cy - 2, 1.2f, c, bg);
  tft.drawWideLine(cx + 3, cy - 2, cx - 3, cy + 3, 1.2f, c, bg);
  tft.drawWideLine(cx, cy + 5, cx + 3, cy + 2, 1.2f, c, bg);
  tft.drawWideLine(cx + 3, cy + 2, cx - 3, cy - 3, 1.2f, c, bg);
}

void getTinyTime(char *b, size_t n)
{
  struct tm ti;
  if (getLocalTime(&ti, 0))
    snprintf(b, n, "%02d:%02d", ti.tm_hour, ti.tm_min);
  else
    snprintf(b, n, "--:--");
}

void drawTopBarTime(uint16_t accent)
{
  char tm[8];
  getTinyTime(tm, sizeof(tm));
  fillBg(236, 3, 82, 21);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_FG);
  tft.drawString(tm, 292, 13, 2);
  drawWifiIcon(298, 8, accent);
  if (bleEnabled)
    drawBtIcon(243, 13, bleConnected ? accent : C_FAINT);
  tft.setTextDatum(TL_DATUM);
}

void updateTopBarClock()
{
  struct tm ti;
  bool timeOk = getLocalTime(&ti, 0);
  if (!bleTopDirty && (!timeOk || ti.tm_min == lastTopBarMinute))
    return;
  bleTopDirty = false;
  if (timeOk)
    lastTopBarMinute = ti.tm_min;
  drawTopBarTime(themeAccent);
}

// Testata: barretta d'accento + titolo pagina a sinistra, ora e WiFi a destra.
// Testata: punto luminoso + titolo a sinistra, ora / Bluetooth / WiFi a destra,
// filetto d'accento che sfuma verso destra.
void drawTopBar(const char *label, uint16_t accent)
{
  fillBg(0, 0, 320, 28);
  glowDisc(17, 13, 3, accent, 4);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_FG);
  tft.drawString(label, 27, 13, 2);
  drawTopBarTime(accent);
  uint16_t lineBg = bgAt(26);
  tft.fillRectHGradient(12, 26, 170, 1, mix565(lineBg, accent, 200), lineBg);
  tft.setTextDatum(TL_DATUM);
}

// Angoli arrotondati "a ritaglio" su immagini gia' disegnate (copertine, radar).
// Angoli arrotondati "a ritaglio" su immagini gia' disegnate (copertine, radar).
// Gli angoli superiori e inferiori possono avere colori diversi (sfondo sfumato).
void roundCorners(int x, int y, int w, int h, int r, uint16_t outside, int32_t outsideBottom = -1)
{
  uint16_t bottom = outsideBottom < 0 ? outside : (uint16_t)outsideBottom;
  for (int i = 0; i < r; i++)
  {
    float dy = r - i - 0.5f;
    int inset = r - (int)floorf(sqrtf((float)r * r - dy * dy) + 0.5f);
    if (inset <= 0)
      continue;
    tft.fillRect(x, y + i, inset, 1, outside);
    tft.fillRect(x + w - inset, y + i, inset, 1, outside);
    tft.fillRect(x, y + h - 1 - i, inset, 1, bottom);
    tft.fillRect(x + w - inset, y + h - 1 - i, inset, 1, bottom);
  }
}

void drawChevron(int cx, int cy, bool left, uint16_t c, uint16_t bg)
{
  int d = left ? -1 : 1;
  tft.drawWideLine(cx - 2 * d, cy - 5, cx + 2 * d, cy, 2.4f, c, bg);
  tft.drawWideLine(cx + 2 * d, cy, cx - 2 * d, cy + 5, 2.4f, c, bg);
}

void drawRefreshIcon(int cx, int cy, uint16_t c, uint16_t bg)
{
  ring(cx, cy, 6, 2, c, bg);
  tft.fillRect(cx + 1, cy - 8, 8, 8, bg); // apre il varco in alto a destra
  tft.fillTriangle(cx + 1, cy - 9, cx + 1, cy - 2, cx + 7, cy - 6, c);
}

// Icone 16x16 della tab bar.
void drawTabIcon(int i, int cx, int cy, uint16_t c, uint16_t bg)
{
  switch (i)
  {
  case 0: // playlist
    for (int k = -1; k <= 1; k++)
    {
      tft.fillCircle(cx - 6, cy + k * 5, 1, c);
      tft.fillRect(cx - 2, cy + k * 5 - 1, 9, 2, c);
    }
    break;
  case 1: // nota musicale
    tft.fillSmoothCircle(cx - 4, cy + 5, 3, c, bg);
    tft.fillSmoothCircle(cx + 5, cy + 3, 3, c, bg);
    tft.fillRect(cx - 2, cy - 6, 2, 12, c);
    tft.fillRect(cx + 7, cy - 8, 2, 12, c);
    tft.drawWideLine(cx - 1, cy - 6, cx + 8, cy - 8, 2.2f, c, bg);
    break;
  case 2: // campanella (notifiche)
    tft.fillSmoothCircle(cx, cy - 1, 6, c, bg);
    tft.fillRect(cx - 6, cy - 1, 13, 6, c);
    tft.fillRect(cx - 8, cy + 4, 17, 2, c);
    tft.fillSmoothCircle(cx, cy + 7, 2, c, bg);
    tft.fillRect(cx - 1, cy - 9, 3, 2, c);
    break;
  case 3: // nuvola
    tft.fillSmoothCircle(cx - 4, cy + 2, 4, c, bg);
    tft.fillSmoothCircle(cx + 1, cy - 2, 5, c, bg);
    tft.fillSmoothCircle(cx + 5, cy + 2, 4, c, bg);
    tft.fillRect(cx - 4, cy + 2, 10, 5, c);
    break;
  case 4: // casa
    tft.fillTriangle(cx - 8, cy, cx, cy - 8, cx + 8, cy, c);
    tft.fillRect(cx - 5, cy, 11, 8, c);
    tft.fillRect(cx - 1, cy + 3, 3, 5, bg);
    break;
  case 5: // radar
    tft.drawSmoothCircle(cx, cy, 7, c, bg);
    tft.drawSmoothCircle(cx, cy, 3, c, bg);
    tft.drawWideLine(cx, cy, cx + 5, cy - 5, 1.8f, c, bg);
    break;
  case 6: // bandiera a scacchi
    tft.fillRect(cx - 7, cy - 8, 2, 17, c);
    for (int r = 0; r < 3; r++)
      for (int k = 0; k < 4; k++)
        if ((r + k) % 2 == 0)
          tft.fillRect(cx - 4 + k * 3, cy - 8 + r * 3, 3, 3, c);
    break;
  case 8: // microfono (controllo PC)
    tft.fillSmoothRoundRect(cx - 3, cy - 8, 7, 11, 3, c, bg);
    tft.drawWideLine(cx - 6, cy - 1, cx - 5, cy + 2, 1.6f, c, bg);
    tft.drawWideLine(cx + 6, cy - 1, cx + 5, cy + 2, 1.6f, c, bg);
    tft.drawWideLine(cx - 5, cy + 2, cx, cy + 5, 1.6f, c, bg);
    tft.drawWideLine(cx + 5, cy + 2, cx, cy + 5, 1.6f, c, bg);
    tft.fillRect(cx, cy + 5, 1, 3, c);
    tft.fillRect(cx - 3, cy + 8, 7, 1, c);
    break;
  default: // calendario
    tft.drawRoundRect(cx - 7, cy - 6, 15, 14, 3, c);
    tft.fillRect(cx - 7, cy - 3, 15, 2, c);
    tft.fillRect(cx - 4, cy - 8, 2, 4, c);
    tft.fillRect(cx + 3, cy - 8, 2, 4, c);
    tft.fillRect(cx - 4, cy + 1, 2, 2, c);
    tft.fillRect(cx, cy + 1, 2, 2, c);
    tft.fillRect(cx + 4, cy + 1, 2, 2, c);
    tft.fillRect(cx - 4, cy + 4, 2, 2, c);
    tft.fillRect(cx, cy + 4, 2, 2, c);
    break;
  }
}

void centerMsg(const String &l1, const String &l2 = "", uint16_t col = C_FG)
{
  bootStatic = false;
  uint16_t accent = (col == C_FG) ? C_AMBER : col;
  applyTheme(accent, false);
  drawBackdrop();
  int h = l2.length() ? 96 : 72;
  int y = (240 - h) / 2;
  panel(24, y, 272, h, 16);
  rrect(24, y + 16, 4, h - 32, 2, accent, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, C_PANEL);
  tft.drawString(l1, 162, l2.length() ? y + 34 : y + h / 2, 4);
  if (l2.length())
  {
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString(l2, 162, y + 68, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

// ########==================== SERVIZIO SPOTIFY - AUTH + API COMUNE ====================########
static const char *REDIRECT_ENC = "http%3A%2F%2F127.0.0.1%3A8888%2Fcallback";
static const char *SCOPES = "user-read-playback-state%20user-modify-playback-state%20user-read-currently-playing%20playlist-read-private%20playlist-read-collaborative";

String accessToken, refreshToken;
uint32_t tokenExpiry = 0;

String basicAuth()
{
  String s = String(SPOTIFY_CLIENT_ID) + ":" + SPOTIFY_CLIENT_SECRET;
  unsigned char out[160];
  size_t olen = 0;
  mbedtls_base64_encode(out, sizeof(out) - 1, &olen, (const unsigned char *)s.c_str(), s.length());
  out[olen] = 0;
  return String((char *)out);
}

bool tokenRequest(const String &body)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  if (!http.begin(client, "https://accounts.spotify.com/api/token"))
    return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", "Basic " + basicAuth());
  int code = http.POST(body);
  if (code != 200)
  {
    Serial.printf("token HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e)
    return false;
  accessToken = doc["access_token"].as<String>();
  int exp = doc["expires_in"] | 3600;
  tokenExpiry = millis() + (uint32_t)(exp - 60) * 1000UL;
  if (doc["refresh_token"].is<const char *>())
  {
    refreshToken = doc["refresh_token"].as<String>();
    prefs.putString("rt", refreshToken);
  }
  return accessToken.length() > 0;
}

bool ensureToken()
{
  if (accessToken.length() && (int32_t)(millis() - tokenExpiry) < 0)
    return true;
  if (!refreshToken.length())
    return false;
  return tokenRequest("grant_type=refresh_token&refresh_token=" + refreshToken);
}

// Ritorna il codice HTTP (o <0 in caso di errore). Se out != nullptr e HTTP 200, fa il parse JSON.
int spotifyCall(const char *method, const char *path, JsonDocument *out = nullptr, const JsonDocument *filter = nullptr)
{
#ifdef WOKWI
  return 204; // volume e comandi simulati localmente
#endif
  for (int attempt = 0; attempt < 2; attempt++)
  {
    if (!ensureToken())
      return -1;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(8000);
    if (!http.begin(client, String("https://api.spotify.com") + path))
      return -1;
    http.addHeader("Authorization", "Bearer " + accessToken);
    int code;
    if (!strcmp(method, "GET"))
    {
      code = http.GET();
    }
    else
    {
      http.addHeader("Content-Length", "0");
      code = http.sendRequest(method, String());
    }
    if (code == 401)
    {
      http.end();
      accessToken = "";
      continue;
    }
    if (code == 200 && out)
    {
      DeserializationError e = filter ? deserializeJson(*out, http.getStream(), DeserializationOption::Filter(*filter))
                                      : deserializeJson(*out, http.getStream());
      if (e)
        code = -2;
    }
    http.end();
    return code;
  }
  return -1;
}

// Variante per le API Spotify che richiedono un body JSON, ad esempio
// l'avvio di una playlist tramite context_uri.
int spotifyCallJson(const char *method, const char *path, const String &body)
{
#ifdef WOKWI
  return 204;
#endif
  for (int attempt = 0; attempt < 2; attempt++)
  {
    if (!ensureToken())
      return -1;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(8000);
    if (!http.begin(client, String("https://api.spotify.com") + path))
      return -1;
    http.addHeader("Authorization", "Bearer " + accessToken);
    http.addHeader("Content-Type", "application/json");
    int code = http.sendRequest(method, body);
    if (code == 401)
    {
      http.end();
      accessToken = "";
      continue;
    }
    http.end();
    return code;
  }
  return -1;
}

// ----- Pagina web per collegare l'account (una tantum) -----
WebServer server(80);
bool authDone = false;

void handleRoot()
{
  String url = String("https://accounts.spotify.com/authorize?response_type=code&client_id=") + SPOTIFY_CLIENT_ID +
               "&scope=" + SCOPES + "&redirect_uri=" + REDIRECT_ENC;
  String html = F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;max-width:520px;margin:20px auto;padding:0 12px'>"
                  "<h2>Collega Spotify</h2><ol>"
                  "<li>Nella dashboard Spotify l'app deve avere il Redirect URI:<br><code>http://127.0.0.1:8888/callback</code></li>"
                  "<li><a href='");
  html += url;
  html += F("'>Clicca qui per autorizzare</a></li>"
            "<li>Dopo aver accettato il browser mostrer&agrave; una pagina di errore: &egrave; normale. "
            "Copia l'<b>intero indirizzo</b> dalla barra e incollalo qui sotto.</li></ol>"
            "<form method=POST action=/code>"
            "<input name=code style='width:100%;padding:8px' placeholder='http://127.0.0.1:8888/callback?code=...'>"
            "<button style='margin-top:10px;padding:8px 16px'>Invia</button></form></body>");
  server.send(200, "text/html", html);
}

void handleCode()
{
  String in = server.arg("code");
  in.trim();
  int i = in.indexOf("code=");
  if (i >= 0)
  {
    in = in.substring(i + 5);
    int a = in.indexOf('&');
    if (a >= 0)
      in = in.substring(0, a);
  }
  if (!in.length())
  {
    server.send(400, "text/plain", "Codice mancante");
    return;
  }
  bool ok = tokenRequest("grant_type=authorization_code&code=" + in + "&redirect_uri=" + REDIRECT_ENC);
  if (ok)
  {
    authDone = true;
    server.send(200, "text/html", "<meta charset=utf-8><h2>Fatto!</h2>Puoi chiudere questa pagina.");
  }
  else
  {
    server.send(400, "text/html", "<meta charset=utf-8><h2>Errore</h2>Codice non valido o scaduto. <a href='/'>Riprova</a>");
  }
}

void runAuthMode()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/code", HTTP_POST, handleCode);
  server.begin();
  applyTheme(SP_GREEN, true);
  drawBackdrop();
  drawTopBar("COLLEGA SPOTIFY", SP_GREEN);
  panel(12, 36, 296, 74, 14, SP_PANEL, SP_LINE);
  rrect(12, 52, 4, 42, 2, SP_GREEN, SP_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(SP_MUTED, SP_PANEL);
  tft.drawString("1. Dal telefono (stessa WiFi) apri:", 26, 46, 2);
  tft.setTextColor(SP_GREEN, SP_PANEL);
  tft.drawString("http://" + WiFi.localIP().toString(), 26, 70, 4);
  panel(12, 118, 296, 84, 14, SP_PANEL, SP_LINE);
  tft.setTextColor(SP_MUTED, SP_PANEL);
  tft.drawString("2. Redirect URI da registrare su Spotify:", 26, 128, 2);
  tft.setTextColor(C_FG, SP_PANEL);
  tft.drawString("http://127.0.0.1:8888/callback", 26, 152, 2);
  glowDisc(30, 182, 3, SP_GREEN, 3, SP_PANEL);
  tft.setTextColor(SP_FAINT, SP_PANEL);
  tft.drawString("in attesa di collegamento...", 40, 178, 1);
  while (!authDone)
  {
    server.handleClient();
    delay(5);
  }
  delay(500);
  server.stop();
}

// ########==================== SERVIZIO BLUETOOTH - TASTIERA HID PC ====================########
// Il CYD si presenta al PC come tastiera Bluetooth LE ("CYD Hub"):
// - tasti multimediali (Consumer Control) per volume di sistema +/-, muto audio e play/pausa;
// - scorciatoia Win+Alt+K per il muto del microfono nelle chiamate (Windows 11: Teams, Zoom, ...).
// Sullo stesso collegamento c'e' un servizio di sincronizzazione usato da tools/cyd_sync.py:
// il PC scrive lo stato reale del microfono e le notifiche (Teams, Webex, Slack), il CYD
// notifica i tocchi su MIC. Senza lo script il microfono usa Win+Alt+K con stato presunto.
constexpr const char *BLE_DEVICE_NAME = "CYD Hub";

bool bleEnabled = true;       // dal menu SET, richiede riavvio
bool notifEnabled = true;     // dal menu SET: popup e LED delle notifiche PC
volatile bool bleConnected = false;
volatile bool bleStateDirty = false; // aggiornato dai callback NimBLE (altro task)
volatile bool bleTopDirty = false;   // ridisegna l'icona Bluetooth nella testata
bool micMuted = false;        // stato del microfono (reale se cyd_sync.py e' attivo, altrimenti presunto)
uint32_t micSyncAt = 0;       // ultimo stato ricevuto da cyd_sync.py

// Scritture del PC sul servizio di sincronizzazione: arrivano dal task NimBLE e
// vengono consumate nel loop principale (bleSyncService) sotto syncMutex.
std::mutex syncMutex;
int syncMicState = -1; // ultimo stato del microfono scritto dal PC, -1 = nessuno
std::vector<std::string> syncNotifQueue;

// Bit del report Consumer Control (report ID 2).
enum ConsumerKey : uint8_t
{
  CK_VOL_UP = 1 << 0,
  CK_VOL_DOWN = 1 << 1,
  CK_MUTE = 1 << 2,
  CK_PLAY_PAUSE = 1 << 3,
};

#ifndef WOKWI
// Tastiera (report ID 1, 8 byte) + Consumer Control (report ID 2, 1 byte).
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, // Generic Desktop / Keyboard
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, // modificatori
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             // byte riservato
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, // 6 tasti
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, // Consumer Control
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04,
    0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2, 0x09, 0xCD, // Vol+, Vol-, Mute, Play/Pause
    0x81, 0x02,
    0x95, 0x04, 0x81, 0x01, // padding a 8 bit
    0xC0};

NimBLEHIDDevice *bleHid = nullptr;
NimBLECharacteristic *bleKeyboardInput = nullptr;
NimBLECharacteristic *bleConsumerInput = nullptr;

static const char *SYNC_SVC_UUID = "c7d10000-5f6e-4b8a-9a2d-3c1e0f6b7a90";
static const char *SYNC_MIC_UUID = "c7d10001-5f6e-4b8a-9a2d-3c1e0f6b7a90";   // PC: '0'/'1'; CYD notifica 't' = tocco
static const char *SYNC_NOTIF_UUID = "c7d10002-5f6e-4b8a-9a2d-3c1e0f6b7a90"; // PC: notifica, campi separati da 0x1F
NimBLECharacteristic *bleMicChar = nullptr;

class SyncCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override
  {
    std::string v = c->getValue();
    std::lock_guard<std::mutex> lock(syncMutex);
    if (c == bleMicChar)
      syncMicState = !v.empty() && v[0] == '1';
    else if (syncNotifQueue.size() < 10)
      syncNotifQueue.push_back(v);
  }
};

class BleServerCallbacks : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override
  {
    bleConnected = true;
    bleStateDirty = true;
    bleTopDirty = true;
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override
  {
    bleConnected = false;
    bleStateDirty = true;
    bleTopDirty = true;
  }
};

void bleInit()
{
  if (!bleEnabled)
    return;
  NimBLEDevice::init(BLE_DEVICE_NAME);
  // Bonding + Secure Connections, senza PIN ("just works"): il PC lo ricorda.
  NimBLEDevice::setSecurityAuth(true, false, true);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());
  server->advertiseOnDisconnect(true);

  bleHid = new NimBLEHIDDevice(server);
  bleHid->setManufacturer("CYD Hub");
  bleHid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
  bleHid->setHidInfo(0x00, 0x01);
  bleHid->setReportMap((uint8_t *)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  bleKeyboardInput = bleHid->getInputReport(1);
  bleConsumerInput = bleHid->getInputReport(2);
  bleHid->setBatteryLevel(100);

  static SyncCallbacks syncCb;
  NimBLEService *sync = server->createService(SYNC_SVC_UUID);
  bleMicChar = sync->createCharacteristic(SYNC_MIC_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  bleMicChar->setCallbacks(&syncCb);
  sync->createCharacteristic(SYNC_NOTIF_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(&syncCb);
  sync->start();

  server->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(BLE_DEVICE_NAME);
  adv->setAppearance(HID_KEYBOARD);
  adv->addServiceUUID(bleHid->getHidService()->getUUID());
  adv->enableScanResponse(true);
  adv->start();
  Serial.printf("BLE HID pronto, heap libero %u\n", (unsigned)ESP.getFreeHeap());
}

bool bleSendKeyboard(uint8_t modifiers, uint8_t key)
{
  if (!bleConnected || !bleKeyboardInput)
    return false;
  uint8_t report[8] = {modifiers, 0, key, 0, 0, 0, 0, 0};
  bleKeyboardInput->notify(report, sizeof(report));
  delay(12);
  memset(report, 0, sizeof(report));
  bleKeyboardInput->notify(report, sizeof(report));
  return true;
}

bool bleSendConsumer(uint8_t keys)
{
  if (!bleConnected || !bleConsumerInput)
    return false;
  uint8_t report = keys;
  bleConsumerInput->notify(&report, 1);
  delay(12);
  report = 0;
  bleConsumerInput->notify(&report, 1);
  return true;
}

void bleSyncSendToggle()
{
  bleMicChar->setValue("t");
  bleMicChar->notify();
}
#else
// Wokwi non simula il Bluetooth: i comandi vengono solo registrati sul monitor seriale.
void bleInit() { bleConnected = true; }
bool bleSendKeyboard(uint8_t modifiers, uint8_t key)
{
  Serial.printf("[WOKWI BLE] tasto mod=0x%02X key=0x%02X\n", modifiers, key);
  return true;
}
bool bleSendConsumer(uint8_t keys)
{
  Serial.printf("[WOKWI BLE] consumer 0x%02X\n", keys);
  return true;
}
void bleSyncSendToggle() { Serial.println("[WOKWI BLE] tocco microfono"); }
#endif

bool micSyncLive() { return micSyncAt && millis() - micSyncAt < 3000; }

// Con cyd_sync.py attivo il muto lo applica il PC (stato reale); altrimenti Win+Alt+K,
// interruttore del microfono di Windows 11 nelle chiamate, con stato presunto.
bool pcToggleMic()
{
  if (micSyncLive())
  {
    bleSyncSendToggle();
    return true;
  }
  constexpr uint8_t MOD_LALT = 0x04, MOD_LGUI = 0x08, KEY_K = 0x0E;
  if (!bleSendKeyboard(MOD_LGUI | MOD_LALT, KEY_K))
    return false;
  micMuted = !micMuted;
  return true;
}

// ########==================== PAGINA 2 - SPOTIFY PLAYER ====================########
// ----- Spotify Dark Neon: UI 320x240 -----
String spotifyFit(String s, int maxW, int font = 2)
{
  if (tft.textWidth(s, font) <= maxW)
    return s;
  while (s.length() && tft.textWidth(s + "...", font) > maxW)
    s.remove(s.length() - 1);
  return s + "...";
}

void spotifyHeader(bool library)
{
  if (library)
  {
    drawTopBar("LIBRERIA SPOTIFY", themeAccent);
    return;
  }
  fillBg(0, 0, 320, 28);
  glowDisc(17, 14, 3, themeAccent, 4);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_FG);
  tft.drawString("SPOTIFY", 27, 14, 2);

  // Tasto CODA: riquadro x=175..225, y=4..23 (vedi spTap).
  panel(175, 4, 51, 20, 10, SP_PANEL2, SP_LINE);
  for (int k = 0; k < 3; k++)
  {
    tft.fillRect(183, 9 + k * 4, 2, 2, themeAccent);
    tft.fillRect(187, 9 + k * 4, 7, 2, C_FG);
  }
  tft.setTextColor(C_FG, SP_PANEL2);
  tft.drawString("CODA", 198, 15, 1);

  drawTopBarTime(themeAccent);
  tft.setTextDatum(TL_DATUM);
}

// ----- Spotify: stato + UI -----
constexpr int ART_X = 12, ART_Y = 32, ART_SZ = 150;
constexpr int TXT_X = 174, TXT_W = 134;
constexpr int PB_X = 14, PB_Y = 196, PB_W = 292, PB_H = 4;
constexpr int CTL_Y = 164;

struct Track
{
  String key, title, artist, artUrl;
  bool playing = false;
  uint32_t progress = 0, duration = 0, stamp = 0;
} T;

bool needText = true, needArt = true, needCtl = true, progDirty = true;
bool drawnPlaying = false;
uint32_t nextPoll = 0, lastProg = 0, statusUntil = 0;
String statusMsg;
JsonDocument fCur;

void initFilter()
{
  fCur["is_playing"] = true;
  fCur["progress_ms"] = true;
  fCur["item"]["id"] = true;
  fCur["item"]["name"] = true;
  fCur["item"]["duration_ms"] = true;
  fCur["item"]["artists"][0]["name"] = true;
  fCur["item"]["show"]["name"] = true;
  fCur["item"]["album"]["images"][0]["url"] = true;
  fCur["item"]["album"]["images"][0]["width"] = true;
  fCur["item"]["images"][0]["url"] = true;
  fCur["item"]["images"][0]["width"] = true;
}

uint32_t progressNow() { return T.progress + (T.playing ? millis() - T.stamp : 0); }

void drawTrackText()
{
  const int x = TXT_X, w = TXT_W;
  const uint16_t acc = themeAccent;
  fillBg(169, 30, 151, 109);

  // Stato: equalizzatore acceso in riproduzione, due barre in pausa.
  tft.setTextDatum(TL_DATUM);
  if (T.playing)
  {
    static const uint8_t EQ[3] = {6, 10, 7};
    for (int i = 0; i < 3; i++)
      rrect(x + i * 4, 45 - EQ[i], 3, EQ[i], 1, acc);
    tft.setTextColor(acc);
    tft.drawString("IN RIPRODUZIONE", x + 16, 37, 1);
  }
  else
  {
    rrect(x, 36, 3, 9, 1, SP_MUTED);
    rrect(x + 5, 36, 3, 9, 1, SP_MUTED);
    tft.setTextColor(SP_MUTED);
    tft.drawString("IN PAUSA", x + 14, 37, 1);
  }

  String title = T.title.length() ? T.title : String("Pronto per la musica");
  tft.setTextColor(C_FG);
  if (tft.textWidth(title, 4) <= w)
    tft.drawString(title, x, 54, 4);
  else
  {
    auto lines = wrapText(title, 2, w);
    drawLines(lines, x, 52, 2, 17, 2, C_FG, w);
  }
  if (T.artist.length())
  {
    tft.setTextColor(SP_MUTED);
    tft.drawString(spotifyFit(T.artist, w, 2), x, 92, 2);
  }

  // Gli eventuali messaggi d'errore restano sotto l'artista.
  if (statusMsg.length())
  {
    const uint16_t errorBg = C565(74, 22, 34);
    rrect(x, 116, w, 20, 8, errorBg);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_RED, errorBg);
    tft.drawString(spotifyFit(statusMsg, w - 12, 1), x + 6, 126, 1);
  }
  tft.setTextDatum(TL_DATUM);
}

void drawControls()
{
  fillBg(TXT_X - 4, 139, 146, 52);
  const int cy = CTL_Y;
  const uint16_t acc = themeAccent;
  // precedente
  disc(192, cy, 16, SP_LINE);
  disc(192, cy, 15, SP_PANEL2, SP_LINE);
  tft.fillRect(185, cy - 6, 2, 12, C_FG);
  tft.fillTriangle(189, cy, 198, cy - 6, 198, cy + 6, C_FG);
  // play / pausa: disco d'accento con alone
  glowDisc(241, cy, 19, acc, 6);
  if (T.playing)
  {
    rrect(234, cy - 8, 5, 16, 2, SP_BG, acc);
    rrect(243, cy - 8, 5, 16, 2, SP_BG, acc);
  }
  else
    tft.fillTriangle(236, cy - 9, 236, cy + 9, 251, cy, SP_BG);
  // successivo
  disc(290, cy, 16, SP_LINE);
  disc(290, cy, 15, SP_PANEL2, SP_LINE);
  tft.fillTriangle(283, cy - 6, 283, cy + 6, 292, cy, C_FG);
  tft.fillRect(294, cy - 6, 2, 12, C_FG);
  drawnPlaying = T.playing;
}

void fmtTime(uint32_t ms, char *b)
{
  uint32_t s = ms / 1000;
  snprintf(b, 8, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void drawProgress()
{
  uint32_t p = progressNow();
  if (T.duration && p > T.duration)
    p = T.duration;
  int w = T.duration ? (int)((uint64_t)PB_W * p / T.duration) : 0;
  const uint16_t acc = themeAccent;
  fillBg(0, 191, 320, 20);
  rrect(PB_X, PB_Y, PB_W, PB_H, 2, SP_BAR);
  if (w > 0)
    tft.fillRectHGradient(PB_X, PB_Y, max(4, w), PB_H, mix565(SP_BAR, acc, 110), acc);
  // cursore bianco con alone d'accento
  disc(PB_X + w, PB_Y + 2, 6, mix565(bgAt(PB_Y), acc, 80));
  disc(PB_X + w, PB_Y + 2, 3, C_FG, mix565(bgAt(PB_Y), acc, 80));
  char b[8];
  tft.setTextColor(SP_MUTED);
  tft.setTextDatum(TL_DATUM);
  fmtTime(p, b);
  tft.drawString(b, PB_X, 202, 1);
  tft.setTextDatum(TR_DATUM);
  fmtTime(T.duration, b);
  tft.drawString(b, PB_X + PB_W, 202, 1);
  tft.setTextDatum(TL_DATUM);
  progDirty = false;
}

int jpegDraw(JPEGDRAW *d)
{
  tft.pushImage(ART_X + d->x, ART_Y + d->y, d->iWidth, d->iHeight, d->pPixels);
  return 1;
}

// Colori dell'alone attorno alla copertina, tinti con l'accento corrente.
uint16_t artHaloOuter() { return mix565(bgAt(ART_Y + ART_SZ / 2), themeAccent, 45); }
uint16_t artHaloInner() { return mix565(bgAt(ART_Y + ART_SZ / 2), themeAccent, 95); }

void drawArtFrame()
{
  rrect(ART_X - 4, ART_Y - 4, ART_SZ + 8, ART_SZ + 8, 17, artHaloOuter());
  rrect(ART_X - 2, ART_Y - 2, ART_SZ + 4, ART_SZ + 4, 15, artHaloInner(), artHaloOuter());
}

void drawArtPlaceholder()
{
  panel(ART_X, ART_Y, ART_SZ, ART_SZ, 13, SP_PANEL, SP_LINE, artHaloInner());
  int cx = ART_X + ART_SZ / 2, cy = ART_Y + ART_SZ / 2;
  // Disco stilizzato; niente immagini aggiuntive in RAM.
  disc(cx, cy, 44, SP_PANEL2, SP_PANEL);
  for (int r = 16; r < 44; r += 7)
    tft.drawCircle(cx, cy, r, SP_LINE);
  glowDisc(cx, cy, 9, themeAccent, 4, SP_PANEL2);
  disc(cx, cy, 2, SP_BG, themeAccent);
}

// Dopo il decode JPEG: ritaglia gli angoli e aggiunge un filo di bordo.
// Dopo il decode JPEG: ritaglia gli angoli sull'alone e aggiunge un filo di bordo.
void finishArt()
{
  roundCorners(ART_X, ART_Y, ART_SZ, ART_SZ, 13, artHaloInner());
  tft.drawRoundRect(ART_X, ART_Y, ART_SZ, ART_SZ, 13, mix565(SP_LINE, themeAccent, 90));
}

// Scarica la copertina in RAM (max 90 KB). Il chiamante libera il buffer.
uint8_t *downloadArt(const String &url, int &size)
{
  size = 0;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  if (!http.begin(client, url))
    return nullptr;
  uint8_t *buf = nullptr;
  int code = http.GET();
  int len = http.getSize();
  if (code == 200 && len > 0 && len < 90000)
  {
    buf = (uint8_t *)malloc(len);
    if (buf)
    {
      WiFiClient *s = http.getStreamPtr();
      int got = 0;
      uint32_t t0 = millis();
      while (got < len && millis() - t0 < 8000)
      {
        int av = s->available();
        if (av > 0)
          got += s->readBytes(buf + got, min(av, len - got));
        else if (!s->connected())
          break;
        else
          delay(1);
      }
      if (got == len)
        size = len;
      else
      {
        free(buf);
        buf = nullptr;
      }
    }
  }
  http.end();
  if (!buf)
    Serial.printf("Copertina fallita: HTTP %d, %d B, heap %u, blocco max %u\n", code, len,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  return buf;
}

int artScale(int width)
{
  return (width >= 500) ? JPEG_SCALE_QUARTER : (width >= 250) ? JPEG_SCALE_HALF
                                                               : 0;
}

// Disegna la copertina.
void drawArtJpeg(uint8_t *buf, int size)
{
  if (!jpeg->openRAM(buf, size, jpegDraw))
    return;
  jpeg->decode(0, 0, artScale(jpeg->getWidth()));
  jpeg->close();
}

#ifdef WOKWI
// Solo per il simulatore: nessuna chiamata Spotify reale.
int demoTrackIndex = 0;
#endif

void spotifyPoll()
{
#ifdef WOKWI
  static const char *titles[] = {"Midnight City", "Instant Crush", "Nightcall", "Starlight", "Sunset Lover"};
  static const char *artists[] = {"M83", "Daft Punk", "Kavinsky", "Muse", "Petit Biscuit"};
  demoTrackIndex = (demoTrackIndex + 5) % 5;
  String key = String(demoTrackIndex);
  if (T.key != key)
  {
    T.key = key;
    T.title = titles[demoTrackIndex];
    T.artist = artists[demoTrackIndex];
    T.artUrl = ""; // niente download HTTPS o JPEG demo
    T.duration = 245000;
    T.progress = 41000;
    T.stamp = millis();
    needArt = needText = needCtl = progDirty = true;
  }
  nextPoll = millis() + SPOTIFY_POLL_MS;
  return;
#endif
  JsonDocument doc;
  int code = spotifyCall("GET", "/v1/me/player/currently-playing?additional_types=episode", &doc, &fCur);
  uint32_t now = millis();
  if (code == 200 && !doc["item"].isNull())
  {
    JsonObject it = doc["item"];
    String id = it["id"] | "";
    String name = fold(it["name"] | "");
    String key = id + "|" + name;
    String artist = fold(it["artists"][0]["name"] | (it["show"]["name"] | ""));
    T.playing = doc["is_playing"] | false;
    T.progress = doc["progress_ms"] | 0;
    T.duration = it["duration_ms"] | 0;
    T.stamp = now;
    if (key != T.key)
    {
      T.key = key;
      T.title = name;
      T.artist = artist;
      JsonArray imgs = it["album"]["images"].as<JsonArray>();
      if (imgs.isNull())
        imgs = it["images"].as<JsonArray>();
      int best = 100000;
      T.artUrl = "";
      for (JsonObject im : imgs)
      {
        int d = abs((int)(im["width"] | 0) - 300);
        if (d < best)
        {
          best = d;
          T.artUrl = im["url"] | "";
        }
      }
      needText = needArt = progDirty = true;
    }
    if (T.playing != drawnPlaying)
      needCtl = needText = true;
    nextPoll = now + SPOTIFY_POLL_MS;
  }
  else if (code == 204 || code == 200)
  {
    String nt = (code == 200) ? "Pubblicita" : "";
    if (T.key.length() || T.title != nt)
    {
      T.key = "";
      T.title = nt;
      T.artist = "";
      T.artUrl = "";
      T.playing = false;
      T.progress = 0;
      T.duration = 0;
      needText = needArt = needCtl = progDirty = true;
    }
    nextPoll = now + 5000;
  }
  else if (code == 429)
  {
    nextPoll = now + 15000;
  }
  else
  {
    nextPoll = now + 6000;
  }
}

void setStatus(const char *m)
{
  statusMsg = m;
  statusUntil = millis() + 4000;
  needText = true;
}

void spotifyCommand(const char *method, const char *path)
{
#ifdef WOKWI
  String p(path);
  if (p.indexOf("/next") >= 0)
  {
    demoTrackIndex++;
    T.key = "";
  }
  else if (p.indexOf("/previous") >= 0)
  {
    demoTrackIndex--;
    T.key = "";
  }
  else if (p.endsWith("/pause"))
    T.playing = false;
  else if (p.endsWith("/play"))
    T.playing = true;
  needText = needCtl = progDirty = true;
  nextPoll = 0;
  return;
#endif
  int code = spotifyCall(method, path);
  if (code == 404)
    setStatus("Nessun dispositivo");
  else if (code == 403)
    setStatus("Serve Spotify Premium");
  else if (code < 0)
    setStatus("Errore di rete");
  nextPoll = millis() + 800;
}

void spEnter()
{
  drawBackdrop();
  spotifyHeader(false);
  needText = needArt = needCtl = progDirty = true;
  nextPoll = 0;
  lastProg = 0;
}

void spLoop()
{
  uint32_t now = millis();
  if (statusMsg.length() && timeReached(now, statusUntil))
  {
    statusMsg = "";
    needText = true;
  }
  if (timeReached(now, nextPoll))
    spotifyPoll();
  if (needText)
  {
    drawTrackText();
    needText = false;
  }
  if (needCtl)
  {
    drawControls();
    needCtl = false;
  }
  if (needArt)
  {
    needArt = false;
    int size = 0;
    uint8_t *buf = T.artUrl.length() ? downloadArt(T.artUrl, size) : nullptr;
    // Decoder creato dopo il download: durante l'HTTPS la RAM serve a TLS.
    if (buf && !(jpeg = new (std::nothrow) JPEGDEC))
    {
      Serial.printf("Copertina: memoria insufficiente per il JPEG, blocco max %u\n", (unsigned)ESP.getMaxAllocHeap());
      free(buf);
      buf = nullptr;
    }
    drawArtFrame();
    drawArtPlaceholder();
    if (buf)
    {
      drawArtJpeg(buf, size);
      finishArt();
      free(buf);
      delete jpeg;
      jpeg = nullptr;
    }
  }
  if (now - lastProg >= 500 || progDirty)
  {
    drawProgress();
    lastProg = now;
  }
}

// Fuori dalle pagine Spotify lo stato del player viene letto piu' di rado,
// solo per tenere allineato il LED (pulsa solo se la musica suona davvero).
constexpr uint32_t SPOTIFY_BG_POLL_MS = 30000UL;

void spotifyBackgroundPoll()
{
#ifdef WOKWI
  return;
#endif
  static uint32_t nextBgPoll = 0;
  if (isSpotifyPage(curPage) || !rgbLedEnabled || !refreshToken.length() || WiFi.status() != WL_CONNECTED)
    return;
  uint32_t now = millis();
  if (!timeReached(now, nextBgPoll))
    return;
  nextBgPoll = now + SPOTIFY_BG_POLL_MS;
  spotifyPoll();
}

void spTap(int x, int y)
{
  // CODA e' ora nel riquadro x=175..225, y=4..23.
  if (x >= 175 && x < 226 && y >= 4 && y < 24)
  {
    openSpotifyQueue();
    return;
  }
  // Tap sulla barra di avanzamento = seek (Premium / player attivo).
  if (y >= 192 && y < NAV_Y - 2 && T.duration > 0)
  {
    int seekX = constrain(x - PB_X, 0, PB_W);
    uint32_t dest = (uint64_t)T.duration * seekX / PB_W;
    String path = "/v1/me/player/seek?position_ms=" + String(dest);
    int code = spotifyCall("PUT", path.c_str());
    if (code == 204)
    {
      T.progress = dest;
      T.stamp = millis();
      progDirty = true;
    }
    else if (code < 0)
      setStatus("Errore di rete");
    nextPoll = millis() + 800;
    return;
  }
  if (y < 144 || y >= 193 || x < 172)
    return;
  if (x < 214)
    spotifyCommand("POST", "/v1/me/player/previous");
  else if (x < 267)
  {
    T.progress = progressNow();
    T.stamp = millis();
    T.playing = !T.playing;
    drawControls();
    needText = true;
    spotifyCommand("PUT", T.playing ? "/v1/me/player/play" : "/v1/me/player/pause");
  }
  else
    spotifyCommand("POST", "/v1/me/player/next");
}

// ########==================== PAGINA 1 - PLAYLIST / CODA SPOTIFY ====================########
struct PlaylistItem
{
  String name;
  String uri;
};

struct QueueItem
{
  String title;
  String artist;
  String uri;
};

enum SpotifyLibraryView
{
  LIB_PLAYLISTS,
  LIB_QUEUE
};

SpotifyLibraryView spotifyLibraryView = LIB_PLAYLISTS;

std::vector<PlaylistItem> spotifyPlaylists;
std::vector<QueueItem> spotifyQueue;

bool playlistsLoaded = false;
bool playlistsLoading = false;
String playlistsStatus;
int playlistPage = 0;

bool queueLoaded = false;
bool queueLoading = false;
String queueStatus;
int queueScrollRow = 0;

constexpr int PLAYLIST_COLS = 2;
constexpr int PLAYLIST_ROWS = 3;
constexpr int PLAYLISTS_PER_PAGE = PLAYLIST_COLS * PLAYLIST_ROWS;

// La coda non e' paginata: mostra una finestra di righe e si scorre verticalmente.
constexpr int QUEUE_VISIBLE_ROWS = 4;

constexpr int PL_X = 12, PL_Y = 74, PL_W = 146, PL_H = 38, PL_GX = 4, PL_GY = 4;
constexpr int Q_X = 12, Q_Y = 74, Q_W = 290, Q_H = 30, Q_GY = 4;

void drawPlaylistPage();

String spotifyArtistFromItem(JsonObject item)
{
  const char *artist = item["artists"][0]["name"] | "";
  if (artist && *artist)
    return fold(artist);

  const char *show = item["show"]["name"] | "";
  return fold(show);
}

bool fetchPlaylists()
{
#ifdef WOKWI
  static const char *samples[] = {"Daily Mix", "Elettronica", "Chill Evening", "Rock Classics", "Night Drive", "Deep Focus", "Workout Mix", "Synthwave", "Discover Weekly"};
  spotifyPlaylists.clear();
  for (int i = 0; i < 9; i++)
    spotifyPlaylists.push_back({String(samples[i]), String("spotify:playlist:demo") + String(i)});
  playlistsLoaded = true;
  playlistsLoading = false;
  playlistsStatus = "";
  playlistPage = 0;
  return true;
#endif
  playlistsLoading = true;
  playlistsStatus = "Caricamento...";
  drawPlaylistPage();

  JsonDocument doc;
  JsonDocument filter;
  filter["items"][0]["name"] = true;
  filter["items"][0]["uri"] = true;

  int code = spotifyCall("GET", "/v1/me/playlists?limit=50&offset=0", &doc, &filter);
  playlistsLoading = false;
  spotifyPlaylists.clear();

  if (code == 200)
  {
    JsonArray items = doc["items"].as<JsonArray>();
    for (JsonObject item : items)
    {
      String name = fold(item["name"] | "");
      String uri = item["uri"] | "";
      if (!name.length() || !uri.length())
        continue;
      spotifyPlaylists.push_back({name, uri});
    }
    playlistsLoaded = true;
    playlistPage = 0;
    playlistsStatus = spotifyPlaylists.empty() ? "Nessuna playlist trovata" : "";
    return true;
  }

  playlistsLoaded = false;
  if (code == 403)
    playlistsStatus = "Ricollega Spotify";
  else if (code == 429)
    playlistsStatus = "Troppe richieste";
  else if (code < 0)
  {
    playlistsStatus = "Errore di rete";
  }
  else
    playlistsStatus = "Spotify HTTP " + String(code);
  return false;
}

bool fetchSpotifyQueue()
{
#ifdef WOKWI
  static const char *titles[] = {"Midnight City", "Instant Crush", "Something About Us", "Nightcall", "Starlight", "Intro", "Awake", "Electric Feel", "Sunset Lover", "Tadow"};
  static const char *artists[] = {"M83", "Daft Punk", "Daft Punk", "Kavinsky", "Muse", "The xx", "Tycho", "MGMT", "Petit Biscuit", "Masego"};
  spotifyQueue.clear();
  for (int i = 0; i < 18; i++)
    spotifyQueue.push_back({String(titles[i % 10]), String(artists[i % 10]), String("spotify:track:demo") + String(i)});
  queueLoaded = true;
  queueLoading = false;
  queueStatus = "";
  queueScrollRow = 0;
  return true;
#endif
  queueLoading = true;
  queueStatus = "Caricamento...";
  drawPlaylistPage();

  JsonDocument doc;
  JsonDocument filter;
  filter["queue"][0]["name"] = true;
  filter["queue"][0]["uri"] = true;
  filter["queue"][0]["artists"][0]["name"] = true;
  filter["queue"][0]["show"]["name"] = true;

  int code = spotifyCall("GET", "/v1/me/player/queue", &doc, &filter);
  queueLoading = false;
  spotifyQueue.clear();

  if (code == 200)
  {
    JsonArray items = doc["queue"].as<JsonArray>();
    for (JsonObject item : items)
    {
      String title = fold(item["name"] | "");
      String uri = item["uri"] | "";
      if (!title.length() || !uri.length())
        continue;

      String artist = spotifyArtistFromItem(item);
      spotifyQueue.push_back({title, artist, uri});
    }

    queueLoaded = true;
    queueScrollRow = 0;
    queueStatus = spotifyQueue.empty() ? "Coda vuota" : "";
    return true;
  }

  queueLoaded = false;
  if (code == 403)
    queueStatus = "Serve Spotify Premium";
  else if (code == 429)
    queueStatus = "Troppe richieste";
  else if (code == 204)
    queueStatus = "Nessun player attivo";
  else if (code < 0)
  {
    queueStatus = "Errore di rete";
    Serial.printf("Coda fallita: codice %d, heap %u, blocco max %u\n", code,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }
  else
    queueStatus = "Spotify HTTP " + String(code);
  return false;
}

uint16_t monogramColor(const String &name)
{
  static const uint16_t P[] = {C_GREEN, C_CYAN, C_AMBER, C_VIOLET, C_PINK, C_BLUE};
  uint32_t h = 7;
  for (unsigned i = 0; i < name.length(); i++)
    h = h * 31 + (uint8_t)name[i];
  return P[h % 6];
}

void drawLibraryTabs()
{
  bool pl = spotifyLibraryView == LIB_PLAYLISTS;
  const uint16_t acc = themeAccent;
  // Controllo a segmenti in vetro con la "pillola" d'accento sulla scheda attiva.
  panel(12, 33, 240, 24, 12, SP_PANEL, SP_LINE);
  rrect(pl ? 15 : 131, 36, pl ? 113 : 118, 18, 9, acc, SP_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(pl ? SP_BG : SP_MUTED, pl ? acc : SP_PANEL);
  tft.drawString("PLAYLIST", 71, 45, 2);
  tft.setTextColor(pl ? SP_MUTED : SP_BG, pl ? SP_PANEL : acc);
  tft.drawString("CODA", 190, 45, 2);
  panel(278, 33, 30, 24, 10, SP_PANEL2, SP_LINE);
  drawRefreshIcon(293, 45, acc, SP_PANEL2);
  tft.setTextDatum(TL_DATUM);
}

// Etichetta di sezione a sinistra e contatore / stato a destra (y = 66).
void drawLibraryCaption(const char *label, const String &status, const String &counter)
{
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(SP_FAINT);
  tft.drawString(label, 14, 66, 1);
  tft.setTextDatum(MR_DATUM);
  if (status.length())
  {
    tft.setTextColor(status == "In riproduzione" ? themeAccent : C_RED);
    tft.drawString(spotifyFit(status, 160, 1), 306, 66, 1);
  }
  else
  {
    tft.setTextColor(SP_FAINT);
    tft.drawString(counter, 306, 66, 1);
  }
  tft.setTextDatum(TL_DATUM);
}

void drawLibraryEmpty(const char *title)
{
  panel(28, 92, 264, 76, 14, SP_PANEL, SP_LINE);
  tft.setTextColor(C_FG, SP_PANEL);
  tft.drawCentreString(title, 160, 109, 2);
  tft.setTextColor(SP_MUTED, SP_PANEL);
  tft.drawCentreString("Premi aggiorna per riprovare", 160, 136, 1);
}

void drawPlaylistGrid()
{
  drawLibraryCaption("LE TUE PLAYLIST", playlistsStatus, String((int)spotifyPlaylists.size()) + " LISTE");

  if (playlistsLoading)
  {
    tft.setTextColor(SP_MUTED);
    tft.drawCentreString("Caricamento playlist...", 160, 122, 2);
    return;
  }
  if (spotifyPlaylists.empty())
  {
    drawLibraryEmpty("Nessuna playlist");
    return;
  }

  const uint16_t acc = themeAccent;
  int pagesCount = (spotifyPlaylists.size() + PLAYLISTS_PER_PAGE - 1) / PLAYLISTS_PER_PAGE;
  playlistPage = constrain(playlistPage, 0, max(0, pagesCount - 1));
  int first = playlistPage * PLAYLISTS_PER_PAGE;
  for (int slot = 0; slot < PLAYLISTS_PER_PAGE; ++slot)
  {
    int idx = first + slot;
    if (idx >= (int)spotifyPlaylists.size())
      break;
    int x = PL_X + (slot % PLAYLIST_COLS) * (PL_W + PL_GX);
    int y = PL_Y + (slot / PLAYLIST_COLS) * (PL_H + PL_GY);
    const String &name = spotifyPlaylists[idx].name;
    uint16_t color = monogramColor(name);
    panel(x, y, PL_W, PL_H, 10, SP_PANEL, SP_LINE);
    // Monogramma su tessera sfumata.
    tft.fillRectVGradient(x + 5, y + 5, 28, 28, color, mix565(color, SP_PANEL, 150));
    roundCorners(x + 5, y + 5, 28, 28, 7, SP_PANEL);
    char letter[2] = {name.length() ? name[0] : '?', 0};
    if (letter[0] >= 'a' && letter[0] <= 'z')
      letter[0] -= 32;
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(SP_BG);
    tft.drawString(letter, x + 19, y + 19, 4);
    tft.setTextDatum(TL_DATUM);
    auto lines = wrapText(name, 2, PL_W - 44);
    drawLines(lines, x + 39, y + ((int)lines.size() > 1 ? 5 : 12), 2, 16, 2, C_FG, PL_W - 44);
  }
  if (pagesCount > 1)
  {
    bool prev = playlistPage > 0, next = playlistPage + 1 < pagesCount;
    panel(12, 197, 38, 13, 6, SP_PANEL2, SP_LINE);
    drawChevron(31, 203, true, prev ? acc : SP_FAINT, SP_PANEL2);
    panel(270, 197, 38, 13, 6, SP_PANEL2, SP_LINE);
    drawChevron(289, 203, false, next ? acc : SP_FAINT, SP_PANEL2);
    // indicatore a punti della pagina
    int dots = min(pagesCount, 9);
    int dx = 160 - (dots - 1) * 5;
    for (int i = 0; i < dots; i++)
    {
      bool on = i == min(playlistPage, dots - 1);
      disc(dx + i * 10, 203, on ? 3 : 2, on ? acc : SP_LINE);
    }
  }
}

void drawQueueList()
{
  drawLibraryCaption("PROSSIMI BRANI", queueStatus, String((int)spotifyQueue.size()) + " BRANI");
  if (queueLoading)
  {
    tft.setTextColor(SP_MUTED);
    tft.drawCentreString("Caricamento coda...", 160, 122, 2);
    return;
  }
  if (spotifyQueue.empty())
  {
    drawLibraryEmpty("Coda vuota");
    return;
  }

  const uint16_t acc = themeAccent;
  const uint16_t badge = mix565(SP_PANEL, acc, 60);
  int total = (int)spotifyQueue.size();
  int visible = min(QUEUE_VISIBLE_ROWS, total);
  int maxFirst = max(0, total - visible);
  queueScrollRow = constrain(queueScrollRow, 0, maxFirst);
  for (int slot = 0; slot < visible; ++slot)
  {
    int idx = queueScrollRow + slot;
    int y = Q_Y + slot * (Q_H + Q_GY);
    const QueueItem &item = spotifyQueue[idx];
    panel(Q_X, y, Q_W, Q_H, 9, SP_PANEL, SP_LINE);
    rrect(Q_X + 5, y + 5, 24, 20, 7, badge, SP_PANEL);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(acc, badge);
    tft.drawString(String(idx + 1), Q_X + 17, y + 15, 1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_FG, SP_PANEL);
    tft.drawString(spotifyFit(item.title, 228, 2), Q_X + 36, y + 2, 2);
    tft.setTextColor(SP_MUTED, SP_PANEL);
    tft.drawString(spotifyFit(item.artist, 228, 1), Q_X + 36, y + 19, 1);
    disc(Q_X + Q_W - 13, y + 15, 9, badge, SP_PANEL);
    tft.fillTriangle(Q_X + Q_W - 16, y + 10, Q_X + Q_W - 16, y + 20,
                     Q_X + Q_W - 8, y + 15, acc);
  }
  if (total > visible)
  {
    const int trackH = visible * (Q_H + Q_GY) - Q_GY;
    int thumbH = max(14, trackH * visible / total);
    int thumbY = Q_Y + (trackH - thumbH) * queueScrollRow / maxFirst;
    rrect(309, Q_Y, 3, trackH, 1, SP_BAR);
    rrect(309, thumbY, 3, thumbH, 1, acc, SP_BAR);
  }
}

void drawPlaylistPage()
{
  drawBackdrop();
  spotifyHeader(true);
  drawLibraryTabs();
  if (spotifyLibraryView == LIB_PLAYLISTS)
    drawPlaylistGrid();
  else
    drawQueueList();
  drawNavBar();
}

void playPlaylist(int idx)
{
#ifdef WOKWI
  // Avvia il brano simulato e apri il player senza API.
  demoTrackIndex = (idx + 5) % 5;
  T.key = "";
  T.playing = true;
  switchPage(1);
  return;
#endif
  if (idx < 0 || idx >= (int)spotifyPlaylists.size())
    return;

  JsonDocument bodyDoc;
  bodyDoc["context_uri"] = spotifyPlaylists[idx].uri;
  String body;
  serializeJson(bodyDoc, body);

  playlistsStatus = "Avvio...";
  drawPlaylistPage();
  int code = spotifyCallJson("PUT", "/v1/me/player/play", body);
  if (code == 204)
  {
    playlistsStatus = "In riproduzione";
    nextPoll = 0;
  }
  else if (code == 404)
    playlistsStatus = "Nessun dispositivo";
  else if (code == 403)
    playlistsStatus = "Serve Spotify Premium";
  else if (code == 429)
    playlistsStatus = "Troppe richieste";
  else if (code < 0)
    playlistsStatus = "Errore di rete";
  else
    playlistsStatus = "Spotify HTTP " + String(code);
  drawPlaylistPage();
}

// Spotify non espone un endpoint ufficiale per riordinare/rimuovere liberamente
// gli elementi della queue. Il tap su una riga implementa quindi "riproduci da qui":
// avvia il brano scelto e passa a Spotify gli elementi successivi della fotografia
// della coda appena letta.
void playQueueFrom(int idx)
{
#ifdef WOKWI
  // Avvia il brano simulato e apri il player senza API.
  demoTrackIndex = (idx + 5) % 5;
  T.key = "";
  T.playing = true;
  switchPage(1);
  return;
#endif
  if (idx < 0 || idx >= (int)spotifyQueue.size())
    return;

  JsonDocument bodyDoc;
  JsonArray uris = bodyDoc["uris"].to<JsonArray>();

  int stop = min((int)spotifyQueue.size(), idx + 40);
  for (int i = idx; i < stop; i++)
  {
    if (spotifyQueue[i].uri.length())
      uris.add(spotifyQueue[i].uri);
  }

  String body;
  serializeJson(bodyDoc, body);

  queueStatus = "Avvio...";
  drawPlaylistPage();
  int code = spotifyCallJson("PUT", "/v1/me/player/play", body);

  if (code == 204)
  {
    queueStatus = "In riproduzione";
    nextPoll = 0;
    queueLoaded = false;
  }
  else if (code == 404)
    queueStatus = "Nessun dispositivo";
  else if (code == 403)
    queueStatus = "Serve Spotify Premium";
  else if (code == 429)
    queueStatus = "Troppe richieste";
  else if (code < 0)
    queueStatus = "Errore di rete";
  else
    queueStatus = "Spotify HTTP " + String(code);

  drawPlaylistPage();
}

void playlistEnter()
{
  drawPlaylistPage();

  if (spotifyLibraryView == LIB_PLAYLISTS)
  {
    if (!playlistsLoaded && !playlistsLoading)
    {
      fetchPlaylists();
      drawPlaylistPage();
    }
  }
  else
  {
    if (!queueLoaded && !queueLoading)
    {
      fetchSpotifyQueue();
      drawPlaylistPage();
    }
  }
}

void playlistLoop() {}

void openSpotifyQueue()
{
  spotifyLibraryView = LIB_QUEUE;
  queueLoaded = false;
  switchPage(0);
}

void playlistTap(int x, int y)
{
  if (y >= 31 && y <= 58)
  {
    if (x >= 12 && x <= 129)
    {
      if (spotifyLibraryView != LIB_PLAYLISTS)
      {
        spotifyLibraryView = LIB_PLAYLISTS;
        drawPlaylistPage();
        if (!playlistsLoaded && !playlistsLoading)
          fetchPlaylists();
        drawPlaylistPage();
      }
      return;
    }
    if (x > 129 && x <= 253)
    {
      if (spotifyLibraryView != LIB_QUEUE)
      {
        spotifyLibraryView = LIB_QUEUE;
        drawPlaylistPage();
        if (!queueLoaded && !queueLoading)
          fetchSpotifyQueue();
        drawPlaylistPage();
      }
      return;
    }
    if (x >= 276)
    {
      if (spotifyLibraryView == LIB_PLAYLISTS)
        fetchPlaylists();
      else
        fetchSpotifyQueue();
      drawPlaylistPage();
      return;
    }
  }
  if (spotifyLibraryView == LIB_PLAYLISTS)
  {
    if (spotifyPlaylists.empty())
      return;
    for (int slot = 0; slot < PLAYLISTS_PER_PAGE; ++slot)
    {
      int cx = PL_X + (slot % PLAYLIST_COLS) * (PL_W + PL_GX);
      int cy = PL_Y + (slot / PLAYLIST_COLS) * (PL_H + PL_GY);
      if (x >= cx && x < cx + PL_W && y >= cy && y < cy + PL_H)
      {
        int idx = playlistPage * PLAYLISTS_PER_PAGE + slot;
        if (idx < (int)spotifyPlaylists.size())
          playPlaylist(idx);
        return;
      }
    }
    int pagesCount = (spotifyPlaylists.size() + PLAYLISTS_PER_PAGE - 1) / PLAYLISTS_PER_PAGE;
    if (y >= 196 && y < NAV_Y)
    {
      if (x < 85 && playlistPage > 0)
      {
        --playlistPage;
        drawPlaylistPage();
      }
      else if (x > 235 && playlistPage + 1 < pagesCount)
      {
        ++playlistPage;
        drawPlaylistPage();
      }
    }
    return;
  }
  if (spotifyQueue.empty())
    return;
  int visible = min(QUEUE_VISIBLE_ROWS, (int)spotifyQueue.size());
  for (int slot = 0; slot < visible; ++slot)
  {
    int cy = Q_Y + slot * (Q_H + Q_GY);
    if (x >= Q_X && x < Q_X + Q_W && y >= cy && y < cy + Q_H)
    {
      int idx = queueScrollRow + slot;
      if (idx < (int)spotifyQueue.size())
        playQueueFrom(idx);
      return;
    }
  }
}
// ########==================== PAGINA 3 - METEO ====================########
struct Weather
{
  float t = 0, feels = 0, wind = 0;
  int hum = 0, code = 0, isDay = 1;
  float tmax[4] = {0}, tmin[4] = {0};
  int dcode[4] = {0}, wday[4] = {0};
} W;
bool weatherOk = false;
uint32_t weatherTs = 0, weatherTry = 0;

const char *DAYS_S[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};
const char *DAYS_L[] = {"Domenica", "Lunedi", "Martedi", "Mercoledi", "Giovedi", "Venerdi", "Sabato"};
const char *MONTHS[] = {"Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno", "Luglio",
                        "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre"};

bool fetchWeather()
{
  weatherTry = millis();
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + WEATHER_LAT + "&longitude=" + WEATHER_LON +
               "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=4&timezone=auto&wind_speed_unit=kmh";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  if (!http.begin(client, url))
    return false;
  int code = http.GET();
  if (code != 200)
  {
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e)
    return false;
  JsonObject c = doc["current"];
  W.t = c["temperature_2m"] | 0.0f;
  W.feels = c["apparent_temperature"] | 0.0f;
  W.hum = c["relative_humidity_2m"] | 0;
  W.code = c["weather_code"] | 0;
  W.wind = c["wind_speed_10m"] | 0.0f;
  W.isDay = c["is_day"] | 1;
  for (int i = 0; i < 4; i++)
  {
    W.dcode[i] = doc["daily"]["weather_code"][i] | 0;
    W.tmax[i] = doc["daily"]["temperature_2m_max"][i] | 0.0f;
    W.tmin[i] = doc["daily"]["temperature_2m_min"][i] | 0.0f;
    const char *d = doc["daily"]["time"][i] | "";
    int y = 0, m = 0, dd = 0;
    sscanf(d, "%d-%d-%d", &y, &m, &dd);
    struct tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = dd;
    tmv.tm_hour = 12;
    mktime(&tmv);
    W.wday[i] = tmv.tm_wday;
  }
  weatherOk = true;
  weatherTs = millis();
  return true;
}

enum Kind
{
  K_CLEAR,
  K_PARTLY,
  K_CLOUDY,
  K_FOG,
  K_DRIZZLE,
  K_RAIN,
  K_SNOW,
  K_STORM
};

Kind kindOf(int c)
{
  if (c == 0)
    return K_CLEAR;
  if (c <= 2)
    return K_PARTLY;
  if (c == 3)
    return K_CLOUDY;
  if (c == 45 || c == 48)
    return K_FOG;
  if (c >= 51 && c <= 57)
    return K_DRIZZLE;
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82))
    return K_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86)
    return K_SNOW;
  if (c >= 95)
    return K_STORM;
  return K_CLOUDY;
}

const char *descOf(int c)
{
  switch (c)
  {
  case 0:
    return "Sereno";
  case 1:
    return "Quasi sereno";
  case 2:
    return "Poco nuvoloso";
  case 3:
    return "Coperto";
  case 45:
  case 48:
    return "Nebbia";
  case 51:
  case 53:
  case 55:
  case 56:
  case 57:
    return "Pioviggine";
  case 61:
  case 63:
  case 65:
    return "Pioggia";
  case 66:
  case 67:
    return "Pioggia gelata";
  case 71:
  case 73:
  case 75:
  case 77:
    return "Neve";
  case 80:
  case 81:
  case 82:
    return "Rovesci";
  case 85:
  case 86:
    return "Rovesci di neve";
  case 95:
  case 96:
  case 99:
    return "Temporale";
  }
  return "--";
}

void drawCloud(int cx, int cy, int s, uint16_t col, uint16_t bg)
{
  int r = max(s / 5, 2);
  tft.fillSmoothCircle(cx - r * 3 / 2, cy + r / 2, r, col, bg);
  tft.fillSmoothCircle(cx + r * 3 / 2, cy + r / 2, r, col, bg);
  tft.fillSmoothCircle(cx - r / 4, cy - r / 3, r * 3 / 2, col, bg);
  tft.fillRect(cx - r * 3 / 2, cy + r / 2 - r / 3, r * 3 + 1, r + r / 3 + 1, col);
}

void drawSun(int cx, int cy, int r, uint16_t bg)
{
  for (int a = 0; a < 8; a++)
  {
    float rad = a * PI / 4;
    float c = cosf(rad), s = sinf(rad);
    tft.drawWideLine(cx + c * (r + 3), cy + s * (r + 3), cx + c * (r + 3 + r / 2 + 2), cy + s * (r + 3 + r / 2 + 2),
                     max(r / 5.0f, 1.6f), C_SUN, bg);
  }
  tft.fillSmoothCircle(cx, cy, r, C_SUN, bg);
}

void drawMoon(int cx, int cy, int r, uint16_t bg)
{
  tft.fillSmoothCircle(cx, cy, r, C565(240, 240, 205), bg);
  tft.fillSmoothCircle(cx + r / 2, cy - r / 3, r * 4 / 5, bg, C565(240, 240, 205));
}

void drawIcon(int cx, int cy, int s, int code, bool day, uint16_t bg = C_BG)
{
  Kind k = kindOf(code);
  int r = max(s / 5, 2);
  float lw = max(s / 22.0f, 1.6f);
  switch (k)
  {
  case K_CLEAR:
    if (day)
      drawSun(cx, cy, s / 4, bg);
    else
      drawMoon(cx, cy, s / 3, bg);
    break;
  case K_PARTLY:
    if (day)
      drawSun(cx - r, cy - r, r + r / 3, bg);
    else
      drawMoon(cx - r, cy - r, r + r / 2, bg);
    drawCloud(cx + r / 2, cy + r / 2, s * 4 / 5, C_CLOUD, bg);
    break;
  case K_CLOUDY:
    drawCloud(cx, cy, s, C_DARK, bg);
    break;
  case K_FOG:
    drawCloud(cx, cy - r / 2, s, C_CLOUD, bg);
    for (int i = 0; i < 3; i++)
      tft.drawWideLine(cx - r * 2 + (i % 2) * r / 2, cy + r * 2 + i * (r / 2 + 2), cx + r * 2 + (i % 2) * r / 2, cy + r * 2 + i * (r / 2 + 2), lw, C_DIM, bg);
    break;
  case K_DRIZZLE:
  case K_RAIN:
    drawCloud(cx, cy - r / 2, s, C_DARK, bg);
    for (int i = -1; i <= 1; i++)
    {
      int x = cx + i * r * 6 / 5;
      int n = (k == K_RAIN) ? 2 : 1;
      for (int j = 0; j < n; j++)
        tft.drawWideLine(x, cy + r * 2 + j * (r / 2 + 3), x - r / 3, cy + r * 3 + j * (r / 2 + 3), lw, C_RAIN, bg);
    }
    break;
  case K_SNOW:
    drawCloud(cx, cy - r / 2, s, C_CLOUD, bg);
    for (int i = -1; i <= 1; i++)
    {
      tft.fillSmoothCircle(cx + i * r * 6 / 5, cy + r * 2 + (i == 0 ? r / 2 : 0), max(r / 4, 1) + 1, C_FG, bg);
      tft.fillSmoothCircle(cx + i * r * 6 / 5 - 2, cy + r * 3 + (i == 0 ? 0 : r / 2), max(r / 4, 1), C_FG, bg);
    }
    break;
  case K_STORM:
    drawCloud(cx, cy - r / 2, s, C_DARK, bg);
    tft.fillTriangle(cx + r / 3, cy + r, cx - r / 2, cy + r * 2, cx, cy + r * 2, C_SUN);
    tft.fillTriangle(cx - r / 8, cy + r * 2, cx + r / 2, cy + r * 3 / 2 + r / 4, cx - r / 5, cy + r * 3 + r / 4, C_SUN);
    break;
  }
}

void drawDegree(int x, int y, uint16_t col, int32_t bg = -1)
{
  ring(x, y, 6, 3, col, bg);
}

void weatherEnter()
{
  const uint16_t acc = themeAccent;
  drawBackdrop();
  drawTopBar("METEO", acc);
  tft.setTextDatum(TL_DATUM);
  if (!weatherOk)
  {
    panel(24, 74, 272, 92, 16);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_FG, C_PANEL);
    tft.drawString("Meteo non disponibile", 160, 108, 2);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString("riprovo automaticamente", 160, 134, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  char b[48];
  // ---- hero: citta', temperatura, max/min, descrizione ----
  glowDisc(18, 40, 2, acc, 3);
  tft.setTextColor(C_DIM);
  tft.drawString(upperStr(WEATHER_CITY), 26, 33, 2);
  String temp = String((int)lroundf(W.t));
  tft.setTextColor(C_FG);
  tft.drawString(temp, 12, 52, 6);
  int tw = tft.textWidth(temp, 6);
  drawDegree(17 + tw, 63, C_FG);
  int bx = 24 + tw + 38;
  tft.fillTriangle(bx, 72, bx + 8, 72, bx + 4, 66, C_ORANGE);
  tft.fillTriangle(bx, 86, bx + 8, 86, bx + 4, 92, acc);
  snprintf(b, sizeof(b), "%d`", (int)lroundf(W.tmax[0]));
  tft.setTextColor(C_FG);
  tft.drawString(b, bx + 14, 62, 2);
  snprintf(b, sizeof(b), "%d`", (int)lroundf(W.tmin[0]));
  tft.setTextColor(C_DIM);
  tft.drawString(b, bx + 14, 82, 2);
  tft.setTextColor(acc);
  tft.drawString(descOf(W.code), 14, 102, 2);

  // ---- medaglione con alone e icona ----
  glowDisc(260, 74, 41, mix565(C_PANEL, acc, 110), 7);
  disc(260, 74, 39, C_PANEL, mix565(C_PANEL, acc, 110));
  drawIcon(260, 74, 60, W.code, W.isDay, C_PANEL);

  // ---- metriche ----
  const int xs[3] = {12, 112, 212};
  const char *labels[3] = {"PERCEPITA", "UMIDITA", "VENTO"};
  const uint16_t tags[3] = {C_ORANGE, acc, C_VIOLET};
  String vals[3] = {String((int)lroundf(W.feels)) + "`", String(W.hum) + "%", String((int)lroundf(W.wind)) + " km/h"};
  for (int i = 0; i < 3; i++)
  {
    panel(xs[i], 124, 96, 38, 10);
    rrect(xs[i] + 8, 132, 3, 22, 1, tags[i], C_PANEL);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString(labels[i], xs[i] + 17, 131, 1);
    tft.setTextColor(C_FG, C_PANEL);
    tft.drawString(vals[i], xs[i] + 17, 142, 2);
  }

  // ---- prossimi giorni, con barra dell'escursione termica ----
  float lo = W.tmin[1], hi = W.tmax[1];
  for (int i = 2; i <= 3; i++)
  {
    lo = min(lo, W.tmin[i]);
    hi = max(hi, W.tmax[i]);
  }
  float span = max(1.0f, hi - lo);
  constexpr int BAR_W = 58;
  for (int i = 1; i <= 3; i++)
  {
    int x = 12 + (i - 1) * 100;
    panel(x, 168, 96, 38, 10);
    tft.setTextColor(acc, C_PANEL);
    tft.drawString(upperStr(DAYS_S[W.wday[i]]), x + 9, 173, 1);
    snprintf(b, sizeof(b), "%d`", (int)lroundf(W.tmax[i]));
    tft.setTextColor(C_FG, C_PANEL);
    tft.drawString(b, x + 9, 182, 2);
    snprintf(b, sizeof(b), "%d`", (int)lroundf(W.tmin[i]));
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString(b, x + 37, 182, 2);
    drawIcon(x + 78, 184, 24, W.dcode[i], true, C_PANEL);
    rrect(x + 9, 199, BAR_W, 3, 1, C_TRACK, C_PANEL);
    int x0 = x + 9 + (int)((W.tmin[i] - lo) / span * BAR_W);
    int x1 = x + 9 + (int)((W.tmax[i] - lo) / span * BAR_W);
    tft.fillRectHGradient(x0, 199, max(3, x1 - x0), 3, acc, C_ORANGE);
  }
}

void weatherLoop() {}
void weatherTap(int, int) {}

// ########==================== PAGINA 5 - RADAR PIOGGIA ====================########
struct RadarState
{
  String host;
  String path;
  time_t frameTime = 0;
  bool ok = false;
  uint32_t stamp = 0;
  int lastHttpCode = 0;
  int lastPngError = 0;
  size_t lastBytes = 0;
} radarState;

constexpr int RADAR_VIEW_X = 32;
constexpr int RADAR_VIEW_Y = 38;
constexpr int RADAR_VIEW_W = 256;
constexpr int RADAR_VIEW_H = 160;
constexpr int RADAR_CROP_Y = (256 - RADAR_VIEW_H) / 2;
constexpr int RADAR_ZOOM = 7;
volatile uint32_t radarPaintedPixels = 0;

bool downloadBinary(const String &url, uint8_t *&data, size_t &len, size_t maxSize = 180000)
{
  data = nullptr;
  len = 0;
  radarState.lastHttpCode = 0;
  radarState.lastBytes = 0;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(12000);
  http.useHTTP10(true); // evita transfer chunked: piu affidabile con PNGdec su ESP32
  http.addHeader("User-Agent", HUB_USER_AGENT);
  http.addHeader("Accept", "image/png,*/*;q=0.8");
  if (!http.begin(client, url))
  {
    Serial.println("Radar: http.begin fallito");
    return false;
  }

  int code = http.GET();
  radarState.lastHttpCode = code;
  Serial.printf("Radar GET %d: %s\n", code, url.c_str());
  if (code != HTTP_CODE_OK)
  {
    http.end();
    return false;
  }

  int declared = http.getSize();
  if (declared > 0 && (size_t)declared > maxSize)
  {
    Serial.printf("Radar: Content-Length troppo grande: %d\n", declared);
    http.end();
    return false;
  }

  size_t cap = declared > 0 ? (size_t)declared : 32768;
  cap = min(cap, maxSize);
  data = (uint8_t *)malloc(cap);
  if (!data)
  {
    Serial.printf("Radar: malloc fallita (%u bytes)\n", (unsigned)cap);
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint32_t lastData = millis();
  while (true)
  {
    int av = stream->available();
    if (av > 0)
    {
      size_t need = len + (size_t)av;
      if (need > maxSize)
      {
        Serial.println("Radar: PNG supera maxSize");
        free(data);
        data = nullptr;
        http.end();
        return false;
      }
      if (need > cap)
      {
        size_t newCap = cap * 2;
        if (newCap < need)
          newCap = need;
        if (newCap > maxSize)
          newCap = maxSize;
        uint8_t *grown = (uint8_t *)realloc(data, newCap);
        if (!grown)
        {
          Serial.println("Radar: realloc fallita");
          free(data);
          data = nullptr;
          http.end();
          return false;
        }
        data = grown;
        cap = newCap;
      }
      size_t room = cap - len;
      size_t want = min((size_t)av, room);
      int got = stream->readBytes(data + len, want);
      if (got > 0)
      {
        len += (size_t)got;
        lastData = millis();
      }
    }
    else
    {
      if (declared >= 0 && len >= (size_t)declared)
        break;
      if (!stream->connected())
        break;
      if (millis() - lastData > 4500)
        break;
      delay(1);
    }
  }
  http.end();

  radarState.lastBytes = len;
  // Firma PNG: 89 50 4E 47 0D 0A 1A 0A
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  bool pngSig = len >= 8 && memcmp(data, sig, 8) == 0;
  bool complete = declared < 0 || len == (size_t)declared;
  Serial.printf("Radar: %u bytes, expected=%d, png=%s\n",
                (unsigned)len, declared, pngSig ? "yes" : "no");
  if (!pngSig || !complete)
  {
    free(data);
    data = nullptr;
    len = 0;
    return false;
  }
  return true;
}

bool fetchRadarMeta()
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(10000);
  http.useHTTP10(true);
  http.addHeader("User-Agent", HUB_USER_AGENT);
  if (!http.begin(client, "https://api.rainviewer.com/public/weather-maps.json"))
    return false;
  int code = http.GET();
  Serial.printf("RainViewer meta HTTP %d\n", code);
  if (code != HTTP_CODE_OK)
  {
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err)
  {
    Serial.printf("RainViewer JSON: %s\n", err.c_str());
    return false;
  }

  JsonArray past = doc["radar"]["past"].as<JsonArray>();
  if (past.isNull() || past.size() == 0)
  {
    Serial.println("RainViewer: nessun frame radar passato");
    return false;
  }

  JsonObject frame = past[past.size() - 1];
  radarState.host = String((const char *)(doc["host"] | "https://tilecache.rainviewer.com"));
  radarState.path = String((const char *)(frame["path"] | ""));
  radarState.frameTime = (time_t)(frame["time"] | 0);
  radarState.ok = radarState.host.length() > 0 && radarState.path.length() > 0;
  if (radarState.ok)
    radarState.stamp = millis();
  Serial.printf("RainViewer host=%s path=%s\n", radarState.host.c_str(), radarState.path.c_str());
  return radarState.ok;
}

// Mappa radar: sfumatura blu notte, griglia a punti e anelli di distanza.
constexpr uint16_t RADAR_MAP_TOP = C565(10, 22, 44);
constexpr uint16_t RADAR_MAP_BOT = C565(4, 9, 20);
constexpr uint16_t RADAR_MAP_MID = C565(7, 15, 32);

void drawRadarBase()
{
  const uint16_t acc = themeAccent;
  drawBackdrop();
  drawTopBar("RADAR PIOGGIA", acc);
  panel(RADAR_VIEW_X - 3, RADAR_VIEW_Y - 3, RADAR_VIEW_W + 6, RADAR_VIEW_H + 6, 11, C_PANEL, mix565(C_LINE, acc, 90));
  tft.fillRectVGradient(RADAR_VIEW_X, RADAR_VIEW_Y, RADAR_VIEW_W, RADAR_VIEW_H, RADAR_MAP_TOP, RADAR_MAP_BOT);
  const uint16_t dot = C565(36, 62, 96);
  for (int x = RADAR_VIEW_X + 16; x < RADAR_VIEW_X + RADAR_VIEW_W; x += 32)
    for (int y = RADAR_VIEW_Y + 15; y < RADAR_VIEW_Y + RADAR_VIEW_H; y += 30)
      tft.fillRect(x, y, 2, 2, dot);
  int cx = RADAR_VIEW_X + RADAR_VIEW_W / 2;
  int cy = RADAR_VIEW_Y + RADAR_VIEW_H / 2;
  tft.drawFastHLine(RADAR_VIEW_X + 8, cy, RADAR_VIEW_W - 16, C565(16, 32, 56));
  tft.drawFastVLine(cx, RADAR_VIEW_Y + 6, RADAR_VIEW_H - 12, C565(16, 32, 56));
  tft.drawCircle(cx, cy, 34, mix565(RADAR_MAP_MID, acc, 90));
  tft.drawCircle(cx, cy, 68, mix565(RADAR_MAP_MID, acc, 60));
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(mix565(RADAR_MAP_MID, acc, 150));
  tft.drawString("30 km", cx, cy - 34 - 10, 1);
  tft.drawString("60 km", cx, cy - 68 - 10, 1);
  tft.setTextDatum(TL_DATUM);
  roundCorners(RADAR_VIEW_X, RADAR_VIEW_Y, RADAR_VIEW_W, RADAR_VIEW_H, 7, C_PANEL);
}

int radarPngDraw(PNGDRAW *pDraw)
{
  if (pDraw->y < RADAR_CROP_Y || pDraw->y >= RADAR_CROP_Y + RADAR_VIEW_H)
    return 1;
  if (pDraw->iWidth > 256)
    return 0;

  uint16_t pixels[256];
  uint8_t mask[1 + 256 / 8];
  png->getLineAsRGB565(pDraw, pixels, PNG_RGB565_BIG_ENDIAN, 0xffffffff);

  // RainViewer usa alpha anche per precipitazioni leggere. Una soglia bassa
  // conserva gli echi deboli e lascia trasparente lo sfondo.
  if (!png->getAlphaMask(pDraw, mask, 16))
    return 1;

  uint32_t linePixels = 0;
  for (int i = 0; i < pDraw->iWidth; ++i)
    if (mask[i >> 3] & (0x80 >> (i & 7)))
      linePixels++;
  radarPaintedPixels += linePixels;

  tft.pushMaskedImage(RADAR_VIEW_X,
                      RADAR_VIEW_Y + pDraw->y - RADAR_CROP_Y,
                      pDraw->iWidth, 1, pixels, mask);
  return 1;
}

int radarTileX(double lon, int zoom)
{
  double n = (double)(1UL << zoom);
  int x = (int)floor((lon + 180.0) / 360.0 * n);
  return constrain(x, 0, (1 << zoom) - 1);
}

int radarTileY(double lat, int zoom)
{
  lat = constrain(lat, -85.05112878, 85.05112878);
  double latRad = lat * PI / 180.0;
  double n = (double)(1UL << zoom);
  int y = (int)floor((1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / PI) / 2.0 * n);
  return constrain(y, 0, (1 << zoom) - 1);
}

bool decodeRadarPng(uint8_t *data, size_t len)
{
  radarPaintedPixels = 0;
  radarState.lastPngError = 0;
  png = new (std::nothrow) PNG;
  if (!png)
  {
    Serial.printf("Radar: memoria insufficiente per il PNG, blocco max %u\n", (unsigned)ESP.getMaxAllocHeap());
    return false;
  }
  int rc = png->openRAM(data, (int)len, radarPngDraw);
  if (rc != PNG_SUCCESS)
  {
    radarState.lastPngError = rc;
    Serial.printf("Radar PNG open error=%d\n", rc);
    delete png;
    png = nullptr;
    return false;
  }

  tft.startWrite();
  rc = png->decode(nullptr, PNG_FAST_PALETTE);
  tft.endWrite();
  radarState.lastPngError = rc;
  png->close();
  delete png;
  png = nullptr;
  Serial.printf("Radar PNG decode=%d, painted=%u\n", rc, (unsigned)radarPaintedPixels);
  return rc == PNG_SUCCESS;
}

bool tryRadarUrl(const String &url)
{
  uint8_t *data = nullptr;
  size_t len = 0;
  if (!downloadBinary(url, data, len))
    return false;
  bool ok = decodeRadarPng(data, len);
  free(data);
  return ok;
}

bool fetchAndDrawRadarTile()
{
  if (!radarState.ok && !fetchRadarMeta())
    return false;

  const double lat = atof(WEATHER_LAT);
  const double lon = atof(WEATHER_LON);
  int tx = radarTileX(lon, RADAR_ZOOM);
  int ty = radarTileY(lat, RADAR_ZOOM);

  // Prima scelta: normale tile Web-Mercator x/y, la forma piu standard e robusta.
  String urlXY = radarState.host + radarState.path + "/256/" + String(RADAR_ZOOM) + "/" +
                 String(tx) + "/" + String(ty) + "/2/1_0.png";
  if (tryRadarUrl(urlXY))
    return true;

  // Fallback: widget centrato direttamente sulle coordinate di Imola.
  Serial.println("Radar: fallback lat/lon");
  String urlLL = radarState.host + radarState.path + "/256/" + String(RADAR_ZOOM) + "/" +
                 String(WEATHER_LAT) + "/" + String(WEATHER_LON) + "/2/1_0.png";
  return tryRadarUrl(urlLL);
}

void drawRadarOverlay()
{
  const uint16_t acc = themeAccent;
  int cx = RADAR_VIEW_X + RADAR_VIEW_W / 2;
  int cy = RADAR_VIEW_Y + RADAR_VIEW_H / 2;
  glowDisc(cx, cy, 6, C_FG, 5, mix565(RADAR_MAP_MID, acc, 40), 200);
  disc(cx, cy, 3, acc, C_FG);
  // etichetta citta'
  String city = upperStr(WEATHER_CITY);
  int w = tft.textWidth(city, 1) + 22;
  const uint16_t chip = C565(14, 26, 48);
  rrect(RADAR_VIEW_X + 8, RADAR_VIEW_Y + 8, w, 16, 8, chip, RADAR_MAP_TOP);
  disc(RADAR_VIEW_X + 17, RADAR_VIEW_Y + 16, 3, acc, chip);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_FG, chip);
  tft.drawString(city, RADAR_VIEW_X + 24, RADAR_VIEW_Y + 16, 1);
  roundCorners(RADAR_VIEW_X, RADAR_VIEW_Y, RADAR_VIEW_W, RADAR_VIEW_H, 7, C_PANEL);
  tft.setTextDatum(TL_DATUM);
}

void drawRadarStamp()
{
  fillBg(0, 202, 320, 9);
  tft.setTextDatum(ML_DATUM);
  if (radarState.frameTime)
  {
    struct tm ti;
    localtime_r(&radarState.frameTime, &ti);
    char b[32];
    snprintf(b, sizeof(b), "ultimo frame %02d:%02d", ti.tm_hour, ti.tm_min);
    glowDisc(RADAR_VIEW_X + 4, 206, 2, C_GREEN, 2);
    tft.setTextColor(C_DIM);
    tft.drawString(b, RADAR_VIEW_X + 13, 206, 1);
  }
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_FAINT);
  tft.drawString("RainViewer", RADAR_VIEW_X + RADAR_VIEW_W, 206, 1);
  tft.setTextDatum(TL_DATUM);
}

void radarEnter()
{
#ifdef WOKWI
  drawRadarBase();
  // Vista offline dimostrativa: la pagina rimane navigabile senza RainViewer.
  const int cx = RADAR_VIEW_X + RADAR_VIEW_W / 2;
  const int cy = RADAR_VIEW_Y + RADAR_VIEW_H / 2;
  tft.fillCircle(cx - 65, cy - 40, 29, C_BLUE);
  tft.fillCircle(cx - 65, cy - 40, 15, C_CYAN);
  tft.fillCircle(cx + 64, cy + 32, 23, C_CYAN);
  tft.fillCircle(cx + 64, cy + 32, 9, C_GREEN);
  drawRadarOverlay();
  fillBg(0, 202, 320, 9);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM);
  tft.drawString("RADAR DEMO - IMOLA", 160, 206, 1);
  tft.setTextDatum(TL_DATUM);
  return;
#endif
  drawRadarBase();
  bool stale = !radarState.ok || (millis() - radarState.stamp > RADAR_REFRESH_MS);
  if (stale)
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_DIM);
    tft.drawString("aggiorno radar...", 160, 206, 1);
    tft.setTextDatum(TL_DATUM);
    radarState.ok = false;
    fetchRadarMeta();
  }
  if (!fetchAndDrawRadarTile())
  {
    panel(60, 88, 200, 68, 14, C_PANEL2, C_LINE, RADAR_MAP_MID);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_FG, C_PANEL2);
    tft.drawString("Radar non disponibile", 160, 108, 2);
    tft.setTextColor(C_DIM, C_PANEL2);
    char e[52];
    snprintf(e, sizeof(e), "HTTP %d  PNG %d", radarState.lastHttpCode, radarState.lastPngError);
    tft.drawString(e, 160, 128, 1);
    tft.setTextColor(themeAccent, C_PANEL2);
    tft.drawString("tocca per riprovare", 160, 141, 1);
    tft.setTextDatum(TL_DATUM);
  }
  else
  {
    drawRadarOverlay();
    if (radarPaintedPixels < 24)
    {
      int cy = RADAR_VIEW_Y + RADAR_VIEW_H - 24;
      const uint16_t chip = C565(14, 26, 48);
      rrect(160 - 74, cy, 148, 16, 8, chip, RADAR_MAP_BOT);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(C_DIM, chip);
      tft.drawString("nessuna pioggia rilevata", 160, cy + 8, 1);
      tft.setTextDatum(TL_DATUM);
    }
  }
  drawRadarStamp();
}

void radarLoop() {}
void radarTap(int, int)
{
  radarState.ok = false;
  radarState.stamp = 0;
  radarEnter();
}

// ########==================== PAGINA 6 - SPORT / FORMULA 1 ====================########
struct F1Race
{
  int round = 0;
  String name;
  String locality;
  String country;
  time_t raceEpoch = 0;
  time_t qualifyingEpoch = 0;
  bool sprint = false;
};

F1Race f1Races[3];
int f1Count = 0;
bool f1Ok = false;
uint32_t f1Stamp = 0;

int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

time_t parseUtcIso(const char *date, const char *utcTime)
{
  if (!date || !*date)
    return 0;
  int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
  if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3)
    return 0;
  if (utcTime && *utcTime)
    sscanf(utcTime, "%d:%d:%d", &hh, &mm, &ss);
  int64_t epoch = daysFromCivil(y, (unsigned)m, (unsigned)d) * 86400LL + hh * 3600LL + mm * 60LL + ss;
  return (time_t)epoch;
}

String compactGpName(String s)
{
  s = fold(s.c_str());
  s.replace(" Grand Prix", " GP");
  return s;
}

bool fetchF1Calendar()
{
  struct tm nowTm;
  if (!getLocalTime(&nowTm, 1000))
    return false;
  int year = nowTm.tm_year + 1900;
  String url = String("https://api.jolpi.ca/ergast/f1/") + year + ".json";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(9000);
  http.useHTTP10(true);
  if (!http.begin(client, url))
    return false;
  int code = http.GET();
  if (code != 200)
  {
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["MRData"]["RaceTable"]["Races"][0]["round"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["date"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["time"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Circuit"]["Location"]["locality"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Circuit"]["Location"]["country"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Qualifying"]["date"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Qualifying"]["time"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Sprint"]["date"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err)
    return false;

  time_t now = time(nullptr);
  f1Count = 0;
  JsonArray races = doc["MRData"]["RaceTable"]["Races"].as<JsonArray>();
  for (JsonObject r : races)
  {
    const char *date = r["date"] | "";
    const char *tm = r["time"] | "00:00:00Z";
    time_t raceTime = parseUtcIso(date, tm);
    if (!raceTime || raceTime + 4 * 3600 < now)
      continue;

    F1Race &out = f1Races[f1Count];
    out.round = atoi(r["round"] | "0");
    out.name = compactGpName(String((const char *)(r["raceName"] | "")));
    out.locality = fold(r["Circuit"]["Location"]["locality"] | "");
    out.country = fold(r["Circuit"]["Location"]["country"] | "");
    out.raceEpoch = raceTime;
    out.qualifyingEpoch = parseUtcIso(r["Qualifying"]["date"] | "", r["Qualifying"]["time"] | "00:00:00Z");
    out.sprint = !r["Sprint"]["date"].isNull();
    f1Count++;
    if (f1Count >= 3)
      break;
  }
  f1Ok = f1Count > 0;
  f1Stamp = millis();
  return f1Ok;
}

void formatLocalRaceTime(time_t epoch, char *buf, size_t len)
{
  struct tm ti;
  localtime_r(&epoch, &ti);
  static const char *MON_S[] = {"GEN", "FEB", "MAR", "APR", "MAG", "GIU", "LUG", "AGO", "SET", "OTT", "NOV", "DIC"};
  snprintf(buf, len, "%s %d %s  %02d:%02d", DAYS_S[ti.tm_wday], ti.tm_mday, MON_S[ti.tm_mon], ti.tm_hour, ti.tm_min);
}

void f1CountdownParts(time_t epoch, String &num, String &unit)
{
  long sec = (long)(epoch - time(nullptr));
  if (sec <= 0)
  {
    num = "0";
    unit = "IN CORSO";
  }
  else if (sec < 48L * 3600L)
  {
    long h = max(1L, sec / 3600L);
    num = String(h);
    unit = h == 1 ? "ORA" : "ORE";
  }
  else
  {
    long d = sec / 86400L;
    num = String(d);
    unit = d == 1 ? "GIORNO" : "GIORNI";
  }
}

bool ensureJuveCalendarFresh();
void invalidateJuveCalendar();
void drawSportsPage();

// Schermata d'attesa mentre si scaricano F1 e Juventus.
void drawSportsLoading()
{
  drawBackdrop();
  drawTopBar("SPORT", themeAccent);
  panel(50, 92, 220, 48, 14);
  for (int i = 0; i < 3; i++)
    glowDisc(146 + i * 14, 106, 3, i == 1 ? themeAccent : mix565(C_PANEL, themeAccent, 120), 2, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString("aggiorno F1 e Juventus...", 160, 125, 1);
  tft.setTextDatum(TL_DATUM);
  drawNavBar();
}

void f1Enter()
{
#ifdef WOKWI
  drawBackdrop();
  drawTopBar("SPORT", themeAccent);
  tft.setTextColor(themeAccent);
  tft.drawString("FORMULA 1", 12, 37, 1);
  panel(12, 51, 296, 62, 12, mix565(C_PANEL, themeAccent, 22), mix565(C_LINE, themeAccent, 120));
  tft.setTextColor(C_FG, mix565(C_PANEL, themeAccent, 22));
  tft.drawString("Prossimo Gran Premio", 24, 64, 2);
  tft.setTextColor(C_DIM, mix565(C_PANEL, themeAccent, 22));
  tft.drawString("Countdown / calendario: DEMO", 24, 88, 1);
  tft.setTextColor(C_FG);
  tft.drawString("JUVENTUS", 12, 120, 1);
  panel(12, 135, 296, 69, 10);
  tft.setTextColor(C_FG, C_PANEL);
  tft.drawString("Prossime partite", 24, 148, 2);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString("Demo offline - nessuna API", 24, 172, 1);
  return;
#endif
  bool f1Stale = !f1Ok || (millis() - f1Stamp > F1_REFRESH_MS);

  if (f1Stale)
  {
    drawSportsLoading();
    fetchF1Calendar();
  }

  ensureJuveCalendarFresh();
  drawSportsPage();
}

void f1Loop() {}

void f1Tap(int x, int y)
{
#ifdef WOKWI
  f1Enter();
  drawNavBar();
  return;
#endif
  // Refresh unico per entrambe le sorgenti sportive.
  if (x >= 274 && y >= 26 && y <= 56)
  {
    f1Ok = false;
    f1Stamp = 0;
    invalidateJuveCalendar();
    drawSportsLoading();
    fetchF1Calendar();
    ensureJuveCalendarFresh();
    drawSportsPage();
  }
}

// ########==================== CALIBRAZIONE TOUCH + MENU SET ====================########
// Se non esiste una calibrazione salvata, vengono usati i valori TS_MINX/TS_MAXX/TS_MINY/TS_MAXY
// definiti in config.h.
struct TouchCalibration
{
  bool valid = false;
  int rawLeft = TS_MINX;
  int rawRight = TS_MAXX;
  int rawTop = TS_MINY;
  int rawBottom = TS_MAXY;
};

TouchCalibration touchCal;

// I target non sono esattamente sui bordi: in questo modo sono facili da toccare con il dito.
// La trasformazione viene poi estrapolata anche fino ai bordi reali 0..319 / 0..239.
constexpr int TOUCH_CAL_X0 = 24;
constexpr int TOUCH_CAL_X1 = 295;
constexpr int TOUCH_CAL_Y0 = 36;
constexpr int TOUCH_CAL_Y1 = 204;

bool touchCalibrationLooksValid(int left, int right, int top, int bottom)
{
  return abs(right - left) > 900 && abs(bottom - top) > 700;
}

void loadTouchCalibration()
{
  bool ok = prefs.getBool("tc_ok", false);
  int left = prefs.getInt("tc_l", TS_MINX);
  int right = prefs.getInt("tc_r", TS_MAXX);
  int top = prefs.getInt("tc_t", TS_MINY);
  int bottom = prefs.getInt("tc_b", TS_MAXY);

  touchCal.valid = ok && touchCalibrationLooksValid(left, right, top, bottom);
  if (touchCal.valid)
  {
    touchCal.rawLeft = left;
    touchCal.rawRight = right;
    touchCal.rawTop = top;
    touchCal.rawBottom = bottom;
    Serial.printf("Touch calibration loaded: L=%d R=%d T=%d B=%d\n",
                  left, right, top, bottom);
  }
  else
  {
    touchCal.rawLeft = TS_MINX;
    touchCal.rawRight = TS_MAXX;
    touchCal.rawTop = TS_MINY;
    touchCal.rawBottom = TS_MAXY;
    Serial.println("Touch calibration: using config.h defaults");
  }
}

void saveTouchCalibration()
{
  prefs.putInt("tc_l", touchCal.rawLeft);
  prefs.putInt("tc_r", touchCal.rawRight);
  prefs.putInt("tc_t", touchCal.rawTop);
  prefs.putInt("tc_b", touchCal.rawBottom);
  prefs.putBool("tc_ok", true);
  touchCal.valid = true;
}

void resetTouchCalibration()
{
  prefs.remove("tc_ok");
  prefs.remove("tc_l");
  prefs.remove("tc_r");
  prefs.remove("tc_t");
  prefs.remove("tc_b");
  touchCal.valid = false;
  touchCal.rawLeft = TS_MINX;
  touchCal.rawRight = TS_MAXX;
  touchCal.rawTop = TS_MINY;
  touchCal.rawBottom = TS_MAXY;
}

int calibratedTouchX(int rawX)
{
  if (touchCal.valid)
    return constrain(map(rawX,
                         touchCal.rawLeft, touchCal.rawRight,
                         TOUCH_CAL_X0, TOUCH_CAL_X1),
                     0, 319);

  return constrain(map(rawX, TS_MINX, TS_MAXX, 0, 319), 0, 319);
}

int calibratedTouchY(int rawY)
{
  if (touchCal.valid)
    return constrain(map(rawY,
                         touchCal.rawTop, touchCal.rawBottom,
                         TOUCH_CAL_Y0, TOUCH_CAL_Y1),
                     0, 239);

  return constrain(map(rawY, TS_MINY, TS_MAXY, 0, 239), 0, 239);
}

void waitTouchReleased(uint32_t timeoutMs = 4000)
{
  uint32_t start = millis();
  while (ts.touched() && millis() - start < timeoutMs)
    delay(10);
  delay(70);
}

bool waitMappedTouchTap(int &x, int &y, uint32_t timeoutMs = 0)
{
  waitTouchReleased();
  uint32_t start = millis();

  while (!ts.touched())
  {
    if (timeoutMs && millis() - start > timeoutMs)
      return false;
    delay(10);
  }

  long sumX = 0, sumY = 0;
  int count = 0;
  uint32_t pressedAt = millis();
  while (ts.touched() && millis() - pressedAt < 700)
  {
    TS_Point p = ts.getPoint();
    sumX += p.x;
    sumY += p.y;
    count++;
    delay(10);
  }

  waitTouchReleased();
  if (count <= 0)
    return false;

  x = calibratedTouchX((int)(sumX / count));
  y = calibratedTouchY((int)(sumY / count));
  return true;
}

void drawCalibrationTarget(int x, int y, int step)
{
  applyTheme(C_AMBER, false);
  drawBackdrop();
  drawTopBar("CALIBRA TOUCH", C_AMBER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_FG);
  tft.drawString("Tocca e tieni premuto", 160, 93, 2);
  tft.setTextColor(C_DIM);
  tft.drawString(String("punto ") + String(step) + "/4", 160, 113, 1);

  // indicatore di avanzamento a punti
  for (int i = 0; i < 4; i++)
    disc(145 + i * 10, 128, i < step ? 3 : 2, i < step ? C_AMBER : C_LINE);

  uint16_t bg = bgAt(y);
  tft.drawSmoothCircle(x, y, 13, mix565(bg, C_AMBER, 90), bg);
  tft.drawSmoothCircle(x, y, 10, C_AMBER, bg);
  tft.drawFastHLine(x - 18, y, 37, C_AMBER);
  tft.drawFastVLine(x, y - 18, 37, C_AMBER);
  glowDisc(x, y, 3, C_FG, 3, -1, 200);
  tft.setTextDatum(TL_DATUM);
}

struct RawTouchPoint
{
  int x = 0;
  int y = 0;
};

bool captureRawCalibrationPoint(int screenX, int screenY, int step, RawTouchPoint &out)
{
  drawCalibrationTarget(screenX, screenY, step);
  waitTouchReleased();

  while (!ts.touched())
    delay(10);

  // Ignora l'assestamento iniziale del dito.
  delay(90);

  long sumX = 0, sumY = 0;
  int minX = 32767, maxX = -32768;
  int minY = 32767, maxY = -32768;
  int count = 0;
  uint32_t start = millis();

  while (ts.touched() && millis() - start < 1000 && count < 32)
  {
    TS_Point p = ts.getPoint();
    sumX += p.x;
    sumY += p.y;
    minX = min(minX, (int)p.x);
    maxX = max(maxX, (int)p.x);
    minY = min(minY, (int)p.y);
    maxY = max(maxY, (int)p.y);
    count++;
    delay(12);
  }

  waitTouchReleased();

  if (count < 6)
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_RED);
    tft.drawString("Tocco troppo breve", 160, 150, 2);
    tft.setTextColor(C_DIM);
    tft.drawString("riprova", 160, 170, 1);
    tft.setTextDatum(TL_DATUM);
    delay(900);
    return false;
  }

  // Media scartando un minimo e un massimo per ridurre i picchi del resistivo.
  if (count > 2)
  {
    sumX -= minX + maxX;
    sumY -= minY + maxY;
    count -= 2;
  }

  out.x = (int)(sumX / count);
  out.y = (int)(sumY / count);
  return true;
}

bool runTouchCalibration()
{
  const int sx[4] = {TOUCH_CAL_X0, TOUCH_CAL_X1, TOUCH_CAL_X1, TOUCH_CAL_X0};
  const int sy[4] = {TOUCH_CAL_Y0, TOUCH_CAL_Y0, TOUCH_CAL_Y1, TOUCH_CAL_Y1};
  RawTouchPoint p[4];

  for (int i = 0; i < 4; i++)
  {
    while (!captureRawCalibrationPoint(sx[i], sy[i], i + 1, p[i]))
      ;
  }

  int left = (p[0].x + p[3].x) / 2;
  int right = (p[1].x + p[2].x) / 2;
  int top = (p[0].y + p[1].y) / 2;
  int bottom = (p[3].y + p[2].y) / 2;

  if (!touchCalibrationLooksValid(left, right, top, bottom))
  {
    drawBackdrop();
    drawTopBar("CALIBRA TOUCH", C_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_RED);
    tft.drawString("Calibrazione non valida", 160, 102, 2);
    tft.setTextColor(C_DIM);
    tft.drawString("nessun dato salvato", 160, 126, 1);
    tft.setTextDatum(TL_DATUM);
    delay(1800);
    return false;
  }

  touchCal.rawLeft = left;
  touchCal.rawRight = right;
  touchCal.rawTop = top;
  touchCal.rawBottom = bottom;
  saveTouchCalibration();

  drawBackdrop();
  drawTopBar("CALIBRA TOUCH", C_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_GREEN);
  tft.drawString("CALIBRAZIONE SALVATA", 160, 92, 2);
  tft.setTextColor(C_DIM);
  tft.drawString("rimane in memoria", 160, 116, 1);
  tft.drawString("anche dopo il riavvio", 160, 132, 1);
  tft.setTextDatum(TL_DATUM);

  Serial.printf("Touch calibration saved: L=%d R=%d T=%d B=%d\n",
                left, right, top, bottom);
  delay(1800);
  return true;
}

// Righe del menu SET: y di partenza, altezza comune.
constexpr int SET_ROW_Y[6] = {32, 66, 100, 134, 168, 204};
constexpr int SET_ROW_H = 30;

// Interruttore a slitta: binario d'accento acceso, pomello bianco.
void drawSwitch(int x, int y, bool on, uint16_t accent, uint16_t under)
{
  rrect(x, y, 38, 18, 9, on ? accent : C_TRACK, under);
  int kx = on ? x + 29 : x + 9;
  disc(kx, y + 9, 7, on ? C_FG : C_DIM, on ? accent : C_TRACK);
}

void drawSetToggle(int row, const char *label, bool on)
{
  int y = SET_ROW_Y[row];
  panel(18, y, 284, SET_ROW_H, 10);
  rrect(26, y + 9, 3, 12, 1, on ? C_GREEN : C_FAINT, C_PANEL);
  tft.setTextColor(C_FG, C_PANEL);
  tft.drawString(label, 36, y + 7, 2);
  drawSwitch(252, y + 6, on, C_GREEN, C_PANEL);
}

void drawTouchCalibrationMenu()
{
  applyTheme(C_AMBER, false);
  drawBackdrop();
  drawTopBar("IMPOSTAZIONI", C_AMBER);
  pill(136, 5, 88, 17, "FIRMWARE " HUB_VERSION, C_AMBER, C_PANEL2);

  panel(18, SET_ROW_Y[0], 284, SET_ROW_H, 10);
  rrect(26, SET_ROW_Y[0] + 9, 3, 12, 1, C_AMBER, C_PANEL);
  tft.setTextColor(C_FG, C_PANEL);
  tft.drawString("CALIBRA TOUCH", 36, SET_ROW_Y[0] + 7, 2);
  drawChevron(286, SET_ROW_Y[0] + 15, false, C_DIM, C_PANEL);

  panel(18, SET_ROW_Y[1], 284, SET_ROW_H, 10);
  rrect(26, SET_ROW_Y[1] + 9, 3, 12, 1, touchCal.valid ? C_AMBER : C_FAINT, C_PANEL);
  tft.setTextColor(touchCal.valid ? C_FG : C_FAINT, C_PANEL);
  tft.drawString("RESET TOUCH", 36, SET_ROW_Y[1] + 7, 2);
  pill(226, SET_ROW_Y[1] + 7, 64, 16, touchCal.valid ? "SALVATA" : "DEFAULT",
       touchCal.valid ? C_GREEN : C_FAINT, C_PANEL2, 1, C_PANEL);

  drawSetToggle(2, "LED RGB", rgbLedEnabled);
  drawSetToggle(3, "BLUETOOTH PC", bleEnabled);
  drawSetToggle(4, "NOTIFICHE PC", notifEnabled);

  panel(18, SET_ROW_Y[5], 284, SET_ROW_H, 10, C_PANEL2, C_LINE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_FG, C_PANEL2);
  tft.drawString("CHIUDI", 160, SET_ROW_Y[5] + 15, 2);
  tft.setTextDatum(TL_DATUM);
}

int setMenuRowAt(int x, int y)
{
  if (x < 18 || x > 302)
    return -1;
  for (int i = 0; i < 6; i++)
    if (y >= SET_ROW_Y[i] - 2 && y <= SET_ROW_Y[i] + SET_ROW_H + 2)
      return i;
  return -1;
}

void runTouchCalibrationMenu()
{
  bool done = false;
  while (!done)
  {
    drawTouchCalibrationMenu();
    int x = 0, y = 0;
    if (!waitMappedTouchTap(x, y))
      continue;

    switch (setMenuRowAt(x, y))
    {
    case 0:
      runTouchCalibration();
      break;
    case 1:
      if (!touchCal.valid)
        break;
      resetTouchCalibration();
      drawBackdrop();
      drawTopBar("IMPOSTAZIONI", C_AMBER);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(C_AMBER);
      tft.drawString("CALIBRAZIONE RESET", 160, 114, 2);
      tft.setTextDatum(TL_DATUM);
      delay(750);
      break;
    case 2:
      rgbLedEnabled = !rgbLedEnabled;
      prefs.putBool("led_on", rgbLedEnabled);
      updateRgbLed();
      break;
    case 3:
      // Lo stack Bluetooth si avvia solo al boot: salva e riavvia.
      bleEnabled = !bleEnabled;
      prefs.putBool("ble_on", bleEnabled);
      centerMsg(bleEnabled ? "Bluetooth ON" : "Bluetooth OFF", "riavvio in corso...");
      delay(1200);
      ESP.restart();
      break;
    case 4:
      notifEnabled = !notifEnabled;
      prefs.putBool("notif_on", notifEnabled);
      break;
    case 5:
      done = true;
      break;
    }
  }
  waitTouchReleased();
}

// ########==================== PAGINA 4 - OROLOGIO / HOME ====================########
void drawNavBar();
int lastMin = -1;

// Cifre a sette segmenti "esagonali": segmenti spenti appena visibili, quelli accesi pieni.
constexpr int CLK_DW = 62, CLK_DH = 104, CLK_T = 12, CLK_Y = 48;
int lastDay = -1;
static void hexH(int x0, int x1, int cy, int ht, uint16_t c)
{
  tft.fillRect(x0 + ht, cy - ht, x1 - x0 - 2 * ht + 1, 2 * ht + 1, c);
  tft.fillTriangle(x0, cy, x0 + ht, cy - ht, x0 + ht, cy + ht, c);
  tft.fillTriangle(x1, cy, x1 - ht, cy - ht, x1 - ht, cy + ht, c);
}
static void hexV(int cx, int y0, int y1, int ht, uint16_t c)
{
  tft.fillRect(cx - ht, y0 + ht, 2 * ht + 1, y1 - y0 - 2 * ht + 1, c);
  tft.fillTriangle(cx, y0, cx - ht, y0 + ht, cx + ht, y0 + ht, c);
  tft.fillTriangle(cx, y1, cx - ht, y1 - ht, cx + ht, y1 - ht, c);
}
// Colore "neon al tramonto": giallo in alto, arancio al centro, magenta in basso.

void drawBigDigit(int x, int y, int digit, uint16_t on, uint16_t off)
{
  //   A
  // F   B
  //   G
  // E   C
  //   D
  static const uint8_t segs[10] = {0b1111110, 0b0110000, 0b1101101, 0b1111001, 0b0110011,
                                   0b1011011, 0b1011111, 0b1110000, 0b1111111, 0b1111011};
  const int ht = CLK_T / 2, g = 2;
  int xl = x + ht, xr = x + CLK_DW - ht;
  int yt = y + ht, ym = y + CLK_DH / 2, yb = y + CLK_DH - ht;
  uint8_t s = segs[digit];
  auto col = [&](uint8_t bit)
  { return (s & bit) ? on : off; };
  hexH(xl + g, xr - g, yt, ht, col(0x40));
  hexV(xr, yt + g, ym - g, ht, col(0x20));
  hexV(xr, ym + g, yb - g, ht, col(0x10));
  hexH(xl + g, xr - g, yb, ht, col(0x08));
  hexV(xl, ym + g, yb - g, ht, col(0x04));
  hexV(xl, yt + g, ym - g, ht, col(0x02));
  hexH(xl + g, xr - g, ym, ht, col(0x01));
}

constexpr int CLK_GAP = 6, CLK_COLON_W = 22;
constexpr int CLK_TOTAL_W = CLK_DW * 4 + CLK_GAP * 2 + CLK_COLON_W;
constexpr int CLK_X1 = (320 - CLK_TOTAL_W) / 2;
constexpr int CLK_COLON_X = CLK_X1 + 2 * CLK_DW + CLK_GAP + CLK_COLON_W / 2;


void drawBigClock(int hour, int minute)
{
  const int x2 = CLK_X1 + CLK_DW + CLK_GAP;
  const int x3 = x2 + CLK_DW + CLK_COLON_W;
  const int x4 = x3 + CLK_DW + CLK_GAP;
  const uint16_t off = C565(10, 12, 18);
  fillBg(0, CLK_Y - 2, 320, CLK_DH + 4);
  drawBigDigit(CLK_X1, CLK_Y, hour / 10, C_FG, off);
  drawBigDigit(x2, CLK_Y, hour % 10, C_FG, off);
  drawBigDigit(x3, CLK_Y, minute / 10, C_FG, off);
  drawBigDigit(x4, CLK_Y, minute % 10, C_FG, off);
  rrect(CLK_COLON_X - 5, CLK_Y + 30, 10, 10, 3, C_AMBER);
  rrect(CLK_COLON_X - 5, CLK_Y + 64, 10, 10, 3, C_AMBER);
}

// Riga dei secondi: sottile, sotto le cifre.
// Riga dei secondi: sottile, sotto le cifre.
void drawSecondsBar(int sec)
{
  constexpr int X = 50, W = 220, Y = 140;
  fillBg(X - 6, Y - 2, W + 12, 8);
  rrect(X, Y, W, 3, 1, C_TRACK);
  int w = (int)((long)W * sec / 59);
  if (w > 3)
    rrect(X, Y, w, 3, 1, C_AMBER, C_TRACK);
  tft.fillCircle(X + w, Y + 1, 2, C_AMBER);
}

// Pulsante rapido per il microfono del PC (Win+Alt+K via Bluetooth), accanto a SET.
constexpr int HOME_MIC_X = 140, HOME_MIC_W = 48;

void drawHomeMicPill()
{
  bool live = bleConnected;
  uint16_t fill = !live ? C_PANEL : micMuted ? mix565(C_PANEL, C_RED, 130) : mix565(C_PANEL2, C_PINK, 40);
  uint16_t border = !live ? C_LINE : micMuted ? C_RED : mix565(C_LINE, C_PINK, 120);
  fillBg(HOME_MIC_X - 2, 2, HOME_MIC_W + 4, 22);
  panel(HOME_MIC_X, 4, HOME_MIC_W, 18, 9, fill, border);
  glowDisc(HOME_MIC_X + 9, 13, 2, !live ? C_FAINT : micMuted ? C_FG : C_PINK, 2, fill);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(!live ? C_FAINT : C_FG, fill);
  tft.drawString(micMuted ? "MUTO" : "MIC", HOME_MIC_X + 28, 13, 1);
  tft.setTextDatum(TL_DATUM);
}

void drawClockFull(const tm &ti)
{
  char b[80];
  const uint16_t acc = themeAccent;
  drawBackdrop();
  drawTopBar("HOME", acc);
  panel(194, 4, 38, 18, 9, C_PANEL2, C_LINE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(acc, C_PANEL2);
  tft.drawString("SET", 213, 13, 1);
  tft.setTextDatum(TL_DATUM);
  if (bleEnabled)
    drawHomeMicPill();
  // data: giorno in accento, resto attenuato
  String wd = upperStr(DAYS_L[ti.tm_wday]);
  snprintf(b, sizeof(b), "%d %s %d", ti.tm_mday, MONTHS[ti.tm_mon], ti.tm_year + 1900);
  int w1 = tft.textWidth(wd, 2), w2 = tft.textWidth(b, 2), gap = 10;
  int sx = (320 - (w1 + gap + w2)) / 2;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(acc);
  tft.drawString(wd, sx, 38, 2);
  tft.setTextColor(C_DIM);
  tft.drawString(b, sx + w1 + gap, 38, 2);
  drawBigClock(ti.tm_hour, ti.tm_min);
  // meteo compatto
  panel(12, 156, 296, 52, 14);
  if (weatherOk)
  {
    glowDisc(42, 182, 20, mix565(C_PANEL, C_CYAN, 50), 3, C_PANEL);
    drawIcon(42, 182, 32, W.code, W.isDay, mix565(C_PANEL, C_CYAN, 50));
    String t = String((int)lroundf(W.t));
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_FG, C_PANEL);
    tft.drawString(t, 72, 182, 4);
    drawDegree(72 + tft.textWidth(t, 4) + 5, 175, C_FG, C_PANEL);
    tft.fillRectVGradient(148, 166, 1, 32, C_PANEL, C_LINE);
    tft.setTextColor(C_FG, C_PANEL);
    tft.drawString(descOf(W.code), 160, 172, 2);
    snprintf(b, sizeof(b), "MIN %d`  MAX %d`", (int)lroundf(W.tmin[0]), (int)lroundf(W.tmax[0]));
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString(b, 160, 192, 2);
  }
  else
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString("meteo non disponibile", 160, 182, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void clockEnter()
{
  lastMin = -1;
  lastDay = -1;
}
void clockLoop()
{
  static int lastSec = -1;
  struct tm ti;
  if (!getLocalTime(&ti, 0))
    return;
  if (lastMin < 0 || ti.tm_mday != lastDay)
  {
    // Ingresso, cambio giorno o meteo aggiornato: ridisegno completo.
    drawClockFull(ti);
    drawNavBar();
    lastMin = ti.tm_min;
    lastDay = ti.tm_mday;
    lastSec = ti.tm_sec;
  }
  else if (ti.tm_min != lastMin)
  {
    drawBigClock(ti.tm_hour, ti.tm_min);
    lastMin = ti.tm_min;
    lastSec = ti.tm_sec;
  }
  else if (ti.tm_sec != lastSec)
  {
    // drawSecondsBar(ti.tm_sec);
    lastSec = ti.tm_sec;
  }
  if (bleStateDirty && bleEnabled)
  {
    bleStateDirty = false;
    drawHomeMicPill();
  }
}

void clockTap(int x, int y)
{
  if (bleEnabled && x >= HOME_MIC_X - 4 && x < 190 && y <= 28)
  {
    pcToggleMic();
    drawHomeMicPill();
    return;
  }
  if (x >= 190 && x <= 238 && y <= 28)
  {
    runTouchCalibrationMenu();
    lastMin = -1;
    lastDay = -1;
  }
}
// ########==================== PAGINA 6 - SPORT / JUVENTUS + UI SPORT ====================########
static const char *JUVE_ICS_URL = "https://ics.fixtur.es/v2/juventus.ics";
constexpr uint32_t JUVE_REFRESH_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr int JUVE_MAX_MATCHES = 6;
constexpr int JUVE_ROW_H = 32;
constexpr int JUVE_ROW_GAP = 3;

struct JuveMatch
{
  time_t epoch = 0;
  bool hasTime = true;
  String title;
};

JuveMatch juveMatches[JUVE_MAX_MATCHES];
int juveCount = 0;
bool juveOk = false;
bool juveLoading = false;
uint32_t juveStamp = 0;
String juveStatus;

String decodeIcsText(String s)
{
  s.replace("\\n", " ");
  s.replace("\\N", " ");
  s.replace("\\,", ",");
  s.replace("\\;", ";");
  s.replace("\\\\", "\\");
  return fold(s.c_str());
}

time_t parseIcsDateTime(const String &raw, bool &hasTime)
{
  String v = raw;
  v.trim();

  if (v.length() < 8)
    return 0;

  int y = v.substring(0, 4).toInt();
  int mo = v.substring(4, 6).toInt();
  int d = v.substring(6, 8).toInt();

  hasTime = v.length() >= 13 && v.charAt(8) == 'T';
  int hh = 12, mm = 0, ss = 0;

  if (hasTime)
  {
    hh = v.substring(9, 11).toInt();
    mm = v.substring(11, 13).toInt();
    if (v.length() >= 15)
      ss = v.substring(13, 15).toInt();
  }

  bool utc = v.endsWith("Z");
  if (utc)
  {
    int64_t epoch = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400LL +
                    hh * 3600LL + mm * 60LL + ss;
    return (time_t)epoch;
  }

  struct tm ti = {};
  ti.tm_year = y - 1900;
  ti.tm_mon = mo - 1;
  ti.tm_mday = d;
  ti.tm_hour = hh;
  ti.tm_min = mm;
  ti.tm_sec = ss;
  ti.tm_isdst = -1;
  return mktime(&ti);
}

void storeJuveEvent(time_t epoch, bool hasTime, const String &summary)
{
  if (!epoch || !summary.length() || juveCount >= JUVE_MAX_MATCHES)
    return;

  time_t now = time(nullptr);
  if (epoch + 4 * 3600 < now)
    return;

  juveMatches[juveCount].epoch = epoch;
  juveMatches[juveCount].hasTime = hasTime;
  juveMatches[juveCount].title = decodeIcsText(summary);
  juveCount++;
}

bool fetchJuveCalendar()
{
  juveLoading = true;
  juveStatus = "Scarico calendario...";
  juveCount = 0;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);

  if (!http.begin(client, JUVE_ICS_URL))
  {
    juveLoading = false;
    juveStatus = "Connessione fallita";
    juveOk = false;
    return false;
  }

  http.addHeader("User-Agent", HUB_USER_AGENT);
  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    juveLoading = false;
    juveStatus = "Fixtur.es HTTP " + String(code);
    juveOk = false;
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  stream->setTimeout(4000);

  bool inEvent = false;
  time_t eventEpoch = 0;
  bool eventHasTime = true;
  String eventSummary;
  String pending;

  auto processIcsLine = [&](String line)
  {
    line.trim();
    if (!line.length())
      return;

    if (line == "BEGIN:VEVENT")
    {
      inEvent = true;
      eventEpoch = 0;
      eventHasTime = true;
      eventSummary = "";
      return;
    }

    if (line == "END:VEVENT")
    {
      if (inEvent)
        storeJuveEvent(eventEpoch, eventHasTime, eventSummary);
      inEvent = false;
      return;
    }

    if (!inEvent)
      return;

    int colon = line.indexOf(':');
    if (colon < 0)
      return;

    String key = line.substring(0, colon);
    String val = line.substring(colon + 1);

    if (key.startsWith("DTSTART"))
      eventEpoch = parseIcsDateTime(val, eventHasTime);
    else if (key.startsWith("SUMMARY"))
      eventSummary = val;
  };

  while ((stream->connected() || stream->available()) && juveCount < JUVE_MAX_MATCHES)
  {
    String line = stream->readStringUntil('\n');
    if (line.endsWith("\r"))
      line.remove(line.length() - 1);

    if ((line.startsWith(" ") || line.startsWith("\t")) && pending.length())
    {
      line.remove(0, 1);
      pending += line;
      continue;
    }

    if (pending.length())
      processIcsLine(pending);

    pending = line;
  }

  if (pending.length() && juveCount < JUVE_MAX_MATCHES)
    processIcsLine(pending);

  http.end();

  // Il feed e' normalmente gia' cronologico, ma ordiniamo comunque
  // le partite future per rendere il parser indipendente dall'ordine degli eventi.
  for (int i = 0; i < juveCount - 1; i++)
    for (int j = i + 1; j < juveCount; j++)
      if (juveMatches[j].epoch < juveMatches[i].epoch)
      {
        JuveMatch tmp = juveMatches[i];
        juveMatches[i] = juveMatches[j];
        juveMatches[j] = tmp;
      }

  juveLoading = false;
  juveOk = juveCount > 0;
  juveStamp = millis();
  juveStatus = juveOk ? "" : "Nessuna partita trovata";
  return juveOk;
}

void formatJuveDate(const JuveMatch &m, char *dateBuf, size_t dateLen, char *timeBuf, size_t timeLen)
{
  struct tm ti;
  localtime_r(&m.epoch, &ti);

  static const char *MON_S[] = {"GEN", "FEB", "MAR", "APR", "MAG", "GIU", "LUG", "AGO", "SET", "OTT", "NOV", "DIC"};
  snprintf(dateBuf, dateLen, "%s %d %s", DAYS_S[ti.tm_wday], ti.tm_mday, MON_S[ti.tm_mon]);

  if (m.hasTime)
    snprintf(timeBuf, timeLen, "%02d:%02d", ti.tm_hour, ti.tm_min);
  else
    snprintf(timeBuf, timeLen, "ORA TBD");
}

bool ensureJuveCalendarFresh()
{
  bool stale = !juveOk || (millis() - juveStamp > JUVE_REFRESH_MS);
  if (stale && !juveLoading)
    fetchJuveCalendar();
  return juveOk;
}

void invalidateJuveCalendar()
{
  juveOk = false;
  juveStamp = 0;
}

String fitTextToWidth(String text, int font, int maxW)
{
  if (tft.textWidth(text, font) <= maxW)
    return text;

  while (text.length() > 3 && tft.textWidth(text + "...", font) > maxW)
    text.remove(text.length() - 1);

  return text + "...";
}

void drawSportsJuveRow(int index, int y)
{
  if (index < 0 || index >= juveCount)
    return;

  const JuveMatch &m = juveMatches[index];
  const bool next = index == 0;
  uint16_t fill = next ? mix565(C_PANEL, C_FG, 16) : C_PANEL;
  uint16_t border = next ? mix565(C_LINE, C_FG, 100) : C_LINE;

  // Data/ora su una riga piccola e partita piu' grande sotto.
  panel(12, y, 296, JUVE_ROW_H, 9, fill, border);

  // Badge bianconero a strisce verticali.
  for (int k = 0; k < 6; k++)
    tft.fillRect(19 + k * 2, y + 6, 2, JUVE_ROW_H - 12, (k & 1) ? C565(16, 16, 20) : C_FG);
  roundCorners(19, y + 6, 12, JUVE_ROW_H - 12, 3, fill);

  char dateBuf[24], timeBuf[12];
  formatJuveDate(m, dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));

  String when = String(dateBuf) + "  " + String(timeBuf);
  when = fitTextToWidth(when, 1, 258);
  String match = fitTextToWidth(m.title, 2, 258);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(next ? C_SUN : C_DIM, fill);
  tft.drawString(when, 40, y + 4, 1);

  tft.setTextColor(C_FG, fill);
  tft.drawString(match, 40, y + 15, 2);
}

void drawSportsPage()
{
  const uint16_t acc = themeAccent;
  drawBackdrop();
  drawTopBar("SPORT", acc);

  // Refresh comune F1 + Juventus.
  panel(282, 31, 26, 22, 9, C_PANEL2, C_LINE);
  drawRefreshIcon(295, 42, C_FG, C_PANEL2);

  // --------------------- Formula 1 ---------------------
  rrect(12, 37, 3, 9, 1, acc);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(acc);
  tft.drawString("FORMULA 1", 20, 42, 1);
  tft.setTextDatum(TL_DATUM);

  if (f1Ok && f1Count > 0)
  {
    F1Race &r = f1Races[0];
    uint16_t heroFill = mix565(C_PANEL, acc, 22);
    panel(12, 51, 296, 62, 12, heroFill, mix565(C_LINE, acc, 120));

    // Strisce di velocita' decorative in basso a destra.
    for (int k = 0; k < 4; k++)
    {
      int x0 = 252 + k * 12;
      uint16_t c = mix565(heroFill, acc, 50 + k * 40);
      tft.fillTriangle(x0, 107, x0 + 6, 107, x0 + 12, 97, c);
      tft.fillTriangle(x0, 107, x0 + 12, 97, x0 + 6, 97, c);
    }

    // Blocco countdown.
    uint16_t cdFill = mix565(heroFill, acc, 70);
    rrect(18, 57, 54, 50, 10, cdFill, heroFill);
    String num, unit;
    f1CountdownParts(r.raceEpoch, num, unit);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_FG, cdFill);
    tft.drawString(num, 45, 76, 4);
    tft.setTextColor(mix565(C_FG, acc, 90), cdFill);
    tft.drawString(unit, 45, 99, 1);

    char b[48];
    snprintf(b, sizeof(b), "R%02d  PROSSIMO GP", r.round);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(acc, heroFill);
    tft.drawString(b, 84, 59, 1);

    String raceName = fitTextToWidth(r.name, 2, 206);
    tft.setTextColor(C_FG, heroFill);
    tft.drawString(raceName, 84, 72, 2);

    formatLocalRaceTime(r.raceEpoch, b, sizeof(b));
    tft.setTextColor(C_DIM, heroFill);
    tft.drawString(b, 84, 96, 1);

    if (r.sprint)
      pill(255, 57, 44, 13, "SPR", C_FG, mix565(heroFill, acc, 140), 1, heroFill);
  }
  else
  {
    panel(12, 51, 296, 62, 12);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString("F1 non disponibile", 160, 82, 2);
    tft.setTextDatum(TL_DATUM);
  }

  // --------------------- Juventus ---------------------
  rrect(12, 120, 3, 9, 1, C_FG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_FG);
  tft.drawString("JUVENTUS", 20, 125, 1);
  tft.setTextColor(C_FAINT);
  tft.drawString("PROSSIME PARTITE", 84, 125, 1);
  tft.setTextDatum(TL_DATUM);

  if (juveLoading)
  {
    panel(12, 135, 296, 69, 10);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString("scarico calendario Juve...", 160, 169, 1);
    tft.setTextDatum(TL_DATUM);
  }
  else if (!juveOk || juveCount == 0)
  {
    panel(12, 135, 296, 69, 10);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString("Juve non disponibile", 160, 162, 2);
    tft.setTextColor(C_FAINT, C_PANEL);
    tft.drawString(juveStatus.length() ? juveStatus : "tocca aggiorna", 160, 181, 1);
    tft.setTextDatum(TL_DATUM);
  }
  else
  {
    int shown = min(juveCount, 2);
    for (int i = 0; i < shown; i++)
      drawSportsJuveRow(i, 135 + i * (JUVE_ROW_H + JUVE_ROW_GAP));
  }

  // drawBackdrop() copre tutto lo schermo: la barra inferiore va sempre ridisegnata.
  drawNavBar();
}

// ########==================== PAGINA 7 - CALENDARIO MENSILE ====================########

int calendarYear = 0;
int calendarMonth = 0; // 0 = gennaio

bool isLeapYear(int year)
{
  return (year % 400 == 0) || ((year % 4 == 0) && (year % 100 != 0));
}

int daysInMonth(int year, int month)
{
  static const int days[] = {
      31, 28, 31, 30, 31, 30,
      31, 31, 30, 31, 30, 31};

  if (month == 1 && isLeapYear(year))
    return 29;

  return days[month];
}

// Restituisce:
// 0 = lunedi
// 1 = martedi
// ...
// 6 = domenica
int firstWeekdayOfMonth(int year, int month)
{
  struct tm t = {};

  t.tm_year = year - 1900;
  t.tm_mon = month;
  t.tm_mday = 1;
  t.tm_hour = 12;

  mktime(&t);

  // tm_wday:
  // 0 = domenica ... 6 = sabato
  return (t.tm_wday + 6) % 7;
}

void drawCalendarPage()
{
  const uint16_t acc = themeAccent;
  drawBackdrop();
  drawTopBar("CALENDARIO", acc);
  // titolo mese + anno
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_FG);
  tft.drawString(MONTHS[calendarMonth], 14, 45, 4);
  tft.setTextColor(acc);
  tft.drawString(String(calendarYear), 14 + tft.textWidth(MONTHS[calendarMonth], 4) + 8, 48, 2);
  // frecce
  panel(242, 31, 30, 26, 10, C_PANEL2, C_LINE);
  drawChevron(257, 44, true, C_FG, C_PANEL2);
  panel(278, 31, 30, 26, 10, C_PANEL2, C_LINE);
  drawChevron(293, 44, false, C_FG, C_PANEL2);
  // intestazione giorni dentro una card
  static const char *weekDays[7] = {"L", "M", "M", "G", "V", "S", "D"};
  constexpr int GRID_X = 12, COL_W = 42;
  panel(12, 62, 296, 148, 14);
  for (int col = 0; col < 7; col++)
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col >= 5 ? acc : C_FAINT, C_PANEL);
    tft.drawString(weekDays[col], GRID_X + col * COL_W + COL_W / 2, 73, 1);
  }
  tft.drawFastHLine(20, 81, 280, C_HAIR);
  // giorni
  int firstDay = firstWeekdayOfMonth(calendarYear, calendarMonth);
  int count = daysInMonth(calendarYear, calendarMonth);
  int prevMonth = (calendarMonth + 11) % 12;
  int prevYear = calendarMonth == 0 ? calendarYear - 1 : calendarYear;
  int prevCount = daysInMonth(prevYear, prevMonth);
  struct tm now;
  bool timeOk = getLocalTime(&now, 0);
  int realYear = timeOk ? now.tm_year + 1900 : -1;
  int realMonth = timeOk ? now.tm_mon : -1;
  int realDay = timeOk ? now.tm_mday : -1;
  int rows = (firstDay + count + 6) / 7;
  // 5 righe da 24 px, 6 righe (mesi lunghi) da 20 px: la griglia resta nella card.
  const int rowH = rows > 5 ? 20 : 24;
  const int firstRowY = 95;
  for (int cell = 0; cell < rows * 7; cell++)
  {
    int col = cell % 7, row = cell / 7;
    int x = GRID_X + col * COL_W + COL_W / 2;
    int y = firstRowY + row * rowH;
    int day = cell - firstDay + 1;
    bool inMonth = day >= 1 && day <= count;
    if (day < 1)
      day = prevCount + day;
    else if (day > count)
      day -= count;
    bool today = inMonth && calendarYear == realYear && calendarMonth == realMonth && day == realDay;
    uint16_t txt, under = C_PANEL;
    if (today)
    {
      glowDisc(x, y, 9, acc, 4, C_PANEL);
      txt = C_BG;
      under = acc;
    }
    else if (!inMonth)
      txt = C565(64, 68, 98);
    else
      txt = col >= 5 ? mix565(C_DIM, acc, 130) : C_FG;
    char d[4];
    snprintf(d, sizeof(d), "%d", day);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(txt, under);
    tft.drawString(d, x, y, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void calendarEnter()
{
  struct tm ti;

  if (getLocalTime(&ti, 0))
  {
    calendarYear = ti.tm_year + 1900;
    calendarMonth = ti.tm_mon;
  }

  drawCalendarPage();
}

void calendarLoop()
{
}

void calendarTap(int x, int y)
{
  if (y > 26 && y < 62)
  {
    if (x >= 236 && x < 275)
    {
      calendarMonth--;
      if (calendarMonth < 0)
      {
        calendarMonth = 11;
        calendarYear--;
      }
      drawCalendarPage();
    }
    else if (x >= 275)
    {
      calendarMonth++;
      if (calendarMonth > 11)
      {
        calendarMonth = 0;
        calendarYear++;
      }
      drawCalendarPage();
    }
    else if (x < 200)
    {
      // tocco sul titolo: torna al mese corrente
      calendarEnter();
    }
  }
}
// ########==================== PAGINA 8 - PC / RIUNIONE ====================########
// Controllo del PC via Bluetooth: microfono (Win+Alt+K) e volume di sistema.
// I pulsanti del volume ripetono il comando se tenuti premuti.
enum PcButton
{
  PCB_MIC,
  PCB_VOL_UP,
  PCB_MUTE,
  PCB_PLAY,
  PCB_VOL_DOWN,
  PCB_COUNT
};

struct PcRect
{
  int x, y, w, h;
};

constexpr PcRect PC_BTN[PCB_COUNT] = {
    {12, 58, 140, 150},  // microfono
    {160, 58, 148, 46},  // volume +
    {160, 108, 72, 46},  // muto audio
    {236, 108, 72, 46},  // play / pausa
    {160, 158, 148, 46}, // volume -
};

constexpr uint32_t PC_REPEAT_DELAY_MS = 450;
constexpr uint32_t PC_REPEAT_EVERY_MS = 110;

String pcStatusMsg;
uint32_t pcStatusUntil = 0;
bool pcRepeatUsed = false;
int pcRepeatId = -1;

int pcButtonAt(int x, int y)
{
  for (int i = 0; i < PCB_COUNT; i++)
  {
    const PcRect &r = PC_BTN[i];
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
      return i;
  }
  return -1;
}

void drawSpeakerIcon(int cx, int cy, uint16_t c, uint16_t bg)
{
  tft.fillRect(cx - 9, cy - 4, 5, 8, c);
  tft.fillTriangle(cx - 5, cy - 4, cx + 2, cy - 10, cx + 2, cy + 10, c);
  tft.fillTriangle(cx - 5, cy + 4, cx - 5, cy - 4, cx + 2, cy + 10, c);
}

void drawMicGlyph(int cx, int cy, uint16_t c, uint16_t bg)
{
  tft.fillSmoothRoundRect(cx - 10, cy - 28, 21, 34, 10, c, bg);
  tft.drawSmoothArc(cx, cy - 4, 18, 15, 270, 360, c, bg, true);
  tft.drawSmoothArc(cx, cy - 4, 18, 15, 0, 90, c, bg, true);
  tft.fillRect(cx - 1, cy + 14, 3, 8, c);
  rrect(cx - 10, cy + 21, 21, 3, 1, c, bg);
}

void drawPcButton(int id, bool pressed)
{
  const PcRect &r = PC_BTN[id];
  const uint16_t acc = themeAccent;
  bool online = bleConnected;
  uint16_t fill = pressed ? mix565(C_PANEL2, acc, 100) : C_PANEL;
  uint16_t border = pressed ? acc : C_LINE;
  uint16_t fg = online ? C_FG : C_FAINT;

  if (id == PCB_MIC && online)
  {
    if (micMuted)
    {
      fill = pressed ? mix565(C_PANEL, C_RED, 170) : mix565(C_PANEL, C_RED, 95);
      border = C_RED;
    }
    else if (!pressed)
    {
      fill = mix565(C_PANEL, acc, 26);
      border = mix565(C_LINE, acc, 140);
    }
  }
  panel(r.x, r.y, r.w, r.h, 14, fill, border);

  int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  tft.setTextDatum(MC_DATUM);
  switch (id)
  {
  case PCB_MIC:
  {
    uint16_t micCol = !online ? C_FAINT : micMuted ? C_FG : acc;
    // alone attorno al microfono: rosso se muto, d'accento se attivo
    if (online)
      glowDisc(cx, r.y + 54, 34, mix565(fill, micMuted ? C_RED : acc, 60), 6, fill, 110);
    drawMicGlyph(cx, r.y + 58, micCol, online ? mix565(fill, micMuted ? C_RED : acc, 60) : fill);
    if (micMuted)
      tft.drawWideLine(cx - 24, r.y + 24, cx + 24, r.y + 86, 4.0f, online ? C_RED : C_FAINT,
                       online ? mix565(fill, C_RED, 60) : fill);
    tft.setTextColor(online ? C_DIM : C_FAINT, fill);
    tft.drawString("MICROFONO", cx, r.y + 106, 1);
    tft.setTextColor(!online ? C_FAINT : micMuted ? C_FG : acc, fill);
    tft.drawString(micMuted ? "MUTO" : "ATTIVO", cx, r.y + 126, 4);
    break;
  }
  case PCB_VOL_UP:
  case PCB_VOL_DOWN:
  {
    uint16_t chip = mix565(fill, acc, online ? 60 : 20);
    disc(r.x + 26, cy, 15, chip, fill);
    drawSpeakerIcon(r.x + 29, cy, fg, chip);
    tft.setTextColor(fg, fill);
    tft.drawString("VOLUME", r.x + 82, cy, 2);
    rrect(r.x + r.w - 32, cy - 1, 16, 3, 1, online ? acc : C_FAINT, fill);
    if (id == PCB_VOL_UP)
      rrect(r.x + r.w - 25, cy - 8, 3, 17, 1, online ? acc : C_FAINT, fill);
    break;
  }
  case PCB_MUTE:
    drawSpeakerIcon(cx - 6, cy - 6, fg, fill);
    tft.drawWideLine(cx + 6, cy - 11, cx + 14, cy - 1, 2.0f, online ? acc : C_FAINT, fill);
    tft.drawWideLine(cx + 14, cy - 11, cx + 6, cy - 1, 2.0f, online ? acc : C_FAINT, fill);
    tft.setTextColor(fg, fill);
    tft.drawString("MUTO", cx, cy + 14, 1);
    break;
  case PCB_PLAY:
    tft.fillTriangle(cx - 12, cy - 13, cx - 12, cy + 1, cx - 2, cy - 6, fg);
    tft.fillRect(cx + 3, cy - 13, 3, 14, online ? acc : C_FAINT);
    tft.fillRect(cx + 9, cy - 13, 3, 14, online ? acc : C_FAINT);
    tft.setTextColor(fg, fill);
    tft.drawString("PLAY", cx, cy + 14, 1);
    break;
  }
  tft.setTextDatum(TL_DATUM);
}

void drawPcStatus()
{
  fillBg(0, 28, 320, 28);
  uint16_t dot;
  String txt;
  if (pcStatusMsg.length())
  {
    dot = C_AMBER;
    txt = pcStatusMsg;
  }
  else if (!bleEnabled)
  {
    dot = C_FAINT;
    txt = "Bluetooth disattivato (HOME > SET)";
  }
  else if (bleConnected)
  {
    dot = C_GREEN;
    txt = "Connesso al PC";
  }
  else
  {
    dot = C_AMBER;
    txt = String("In attesa: abbina \"") + BLE_DEVICE_NAME + "\" dal PC";
  }
  glowDisc(19, 42, 3, dot, 3);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_DIM);
  tft.drawString(fitTextToWidth(txt, 2, 284), 30, 42, 2);
  tft.setTextDatum(TL_DATUM);
}

void drawPcPage()
{
  drawBackdrop();
  drawTopBar("PC / RIUNIONE", themeAccent);
  drawPcStatus();
  for (int i = 0; i < PCB_COUNT; i++)
    drawPcButton(i, false);
}

void pcSetStatus(const char *msg)
{
  pcStatusMsg = msg;
  pcStatusUntil = millis() + 2500;
  drawPcStatus();
}

// Invia il comando del pulsante; ritorna false se il PC non e' connesso.
bool pcSendButton(int id)
{
  switch (id)
  {
  case PCB_MIC:
    return pcToggleMic();
  case PCB_VOL_UP:
    return bleSendConsumer(CK_VOL_UP);
  case PCB_VOL_DOWN:
    return bleSendConsumer(CK_VOL_DOWN);
  case PCB_MUTE:
    return bleSendConsumer(CK_MUTE);
  case PCB_PLAY:
    return bleSendConsumer(CK_PLAY_PAUSE);
  }
  return false;
}

void pcEnter()
{
  pcRepeatUsed = false;
  pcRepeatId = -1;
  bleStateDirty = false;
  drawPcPage();
}

void pcLoop()
{
  uint32_t now = millis();
  if (bleStateDirty)
  {
    bleStateDirty = false;
    drawPcPage();
    drawNavBar();
    return;
  }
  if (pcStatusMsg.length() && timeReached(now, pcStatusUntil))
  {
    pcStatusMsg = "";
    drawPcStatus();
  }

  // Auto-repeat del volume: si attiva tenendo il dito fermo sul pulsante.
  static uint32_t lastTouchStart = 0, nextRepeat = 0;
  if (!tsDown)
  {
    // Rilascio dopo una pressione lunga (non e' un tap): ridisegna il pulsante.
    if (pcRepeatId >= 0)
    {
      drawPcButton(pcRepeatId, false);
      pcRepeatId = -1;
      pcRepeatUsed = false;
    }
    return;
  }
  if (tsStartT != lastTouchStart)
  {
    lastTouchStart = tsStartT;
    pcRepeatUsed = false;
  }
  int id = pcButtonAt(sx, sy);
  if (id != PCB_VOL_UP && id != PCB_VOL_DOWN)
    return;
  if (pcButtonAt(lx, ly) != id || now - tsStartT < PC_REPEAT_DELAY_MS || !timeReached(now, nextRepeat))
    return;
  if (!pcRepeatUsed)
    drawPcButton(id, true);
  pcRepeatUsed = true;
  pcRepeatId = id;
  nextRepeat = now + PC_REPEAT_EVERY_MS;
  pcSendButton(id);
}

void pcTap(int x, int y)
{
  int id = pcButtonAt(x, y);
  if (id < 0)
    return;
  if (pcRepeatUsed)
  {
    // Il volume e' gia' stato inviato in ripetizione: al rilascio si ridisegna e basta.
    pcRepeatUsed = false;
    pcRepeatId = -1;
    drawPcButton(id, false);
    return;
  }
  drawPcButton(id, true);
  bool ok = pcSendButton(id);
  delay(70);
  drawPcButton(id, false);
  if (!ok)
    pcSetStatus(bleEnabled ? "PC non connesso" : "Attiva il Bluetooth da HOME > SET");
}

// ########==================== PAGINA - NOTIFICHE PC ====================########
// Le notifiche di Teams, Webex e Slack arrivano da tools/cyd_sync.py via Bluetooth.
// Una notifica nuova appare in un popup per 10 s su qualsiasi pagina; qui c'e' lo storico.
constexpr int NOTIF_PAGE = 2;
constexpr int NOTIF_MAX = 30;
constexpr uint32_t NOTIF_POPUP_MS = 10000;
constexpr int NOTIF_Y = 32, NOTIF_H = 42, NOTIF_GAP = 2, NOTIF_ROWS = 4;
constexpr int POPUP_X = 8, POPUP_Y = 32, POPUP_W = 304, POPUP_H = 52;

struct Notif
{
  uint32_t id;
  String app, title, body, time;
};
std::vector<Notif> notifs;    // la piu' recente in testa
int notifScroll = 0;
bool notifListDirty = false;  // lista cambiata (impostato dall'handler HTTP)
bool notifPopupNew = false;   // notifica nuova da mostrare nel popup
uint32_t notifPopupUntil = 0; // 0 = nessun popup a schermo
uint32_t notifLedUntil = 0;   // lampeggio del LED RGB (vedi updateRgbLed)
uint8_t notifLedRgb[3];
bool notifUnread = false;     // notifiche arrivate dopo l'ultima visita allo storico: puntino sulla dock
bool navDotShown = false;     // stato del puntino attualmente disegnato

// Colore dell'app: Teams viola, Webex verde acqua, Slack bianco, altre arancio.
// rgb (opzionale) riceve la versione satura per il LED.
uint16_t notifAppColor(const String &app, uint8_t *rgb = nullptr)
{
  static const uint8_t LED[4][3] = {{140, 0, 255}, {0, 255, 150}, {255, 255, 255}, {255, 90, 0}};
  static const uint16_t SCREEN[4] = {C_VIOLET, SP_GREEN, C_FG, C_ORANGE};
  String a = app;
  a.toLowerCase();
  int k = a.indexOf("teams") >= 0 ? 0 : a.indexOf("webex") >= 0 ? 1 : a.indexOf("slack") >= 0 ? 2 : 3;
  if (rgb)
    memcpy(rgb, LED[k], 3);
  return SCREEN[k];
}

// old = notifica gia' presente all'avvio dello script: va nello storico senza popup.
void addNotif(uint32_t id, bool old, const String &time, const String &app, const String &title, const String &body)
{
  for (const Notif &n : notifs)
    if (n.id == id)
      return;
  notifs.insert(notifs.begin(), {id, app, title, body, time});
  if (notifs.size() > NOTIF_MAX)
    notifs.pop_back();
  notifListDirty = true;
  if (curPage != NOTIF_PAGE)
    notifUnread = true;
  if (!old && notifEnabled)
  {
    notifPopupNew = true;
    notifAppColor(app, notifLedRgb);
    notifLedUntil = millis() + NOTIF_POPUP_MS;
  }
}

// Chiamato dal loop: applica stato del microfono e notifiche scritti dal PC.
void bleSyncService()
{
  std::vector<std::string> queue;
  int mic;
  {
    std::lock_guard<std::mutex> lock(syncMutex);
    queue.swap(syncNotifQueue);
    mic = syncMicState;
    syncMicState = -1;
  }
  if (mic >= 0)
  {
    micSyncAt = millis();
    if ((bool)mic != micMuted)
    {
      micMuted = mic;
      bleStateDirty = true; // ridisegna pillola HOME / pagina PC
    }
  }
  for (const std::string &raw : queue)
  {
    String f[6]; // id, old, ora, app, titolo, testo
    int k = 0;
    for (char ch : raw)
      if (ch == '\x1f' && k < 5)
        k++;
      else
        f[k] += ch;
    addNotif(f[0].toInt(), f[1] == "1", f[2], fold(f[3].c_str()), fold(f[4].c_str()), fold(f[5].c_str()));
  }
}

// App + ora sulla prima riga, poi titolo e testo: stesso blocco nella lista e nel popup.
void drawNotifText(const Notif &n, int x, int y, int w, uint16_t fill, uint16_t c)
{
  glowDisc(x + 2, y + 4, 2, c, 2, fill);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(c, fill);
  tft.drawString(fitTextToWidth(upperStr(n.app.c_str()), 1, w - 50), x + 10, y, 1);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_FAINT, fill);
  tft.drawString(n.time, x + w, y, 1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_FG, fill);
  tft.drawString(fitTextToWidth(n.title, 2, w), x, y + 11, 2);
  tft.setTextColor(C_DIM, fill);
  tft.drawString(fitTextToWidth(n.body, 1, w), x, y + 29, 1);
}

void drawNotifPopup(const Notif &n)
{
  uint16_t c = notifAppColor(n.app);
  uint16_t fill = mix565(C_PANEL2, c, 30);
  uint16_t under = bgAt(POPUP_Y);
  // cornice color sfondo: stacca il popup dal contenuto della pagina sottostante
  tft.fillRoundRect(POPUP_X - 3, POPUP_Y - 3, POPUP_W + 6, POPUP_H + 6, 16, under);
  panel(POPUP_X, POPUP_Y, POPUP_W, POPUP_H, 14, fill, c, under);
  drawNotifText(n, POPUP_X + 12, POPUP_Y + 7, POPUP_W - 24, fill, c);
}

void drawNotifList()
{
  fillBg(0, NOTIF_Y, 320, NAV_Y - NOTIF_Y);
  if (notifs.empty())
  {
    panel(24, 74, 272, 92, 16);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_FG, C_PANEL);
    tft.drawString("Nessuna notifica", 160, 108, 2);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString(micSyncLive() ? "PC collegato, in attesa" : "avvia tools/cyd_sync.py sul PC", 160, 134, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  for (int i = 0; i < NOTIF_ROWS && notifScroll + i < (int)notifs.size(); i++)
  {
    const Notif &n = notifs[notifScroll + i];
    int y = NOTIF_Y + i * (NOTIF_H + NOTIF_GAP);
    uint16_t c = notifAppColor(n.app);
    panel(12, y, 290, NOTIF_H, 10);
    rrect(12, y + 8, 3, NOTIF_H - 16, 1, c, C_PANEL);
    drawNotifText(n, 24, y + 3, 270, C_PANEL, c);
  }
  // scrollbar laterale, come nella coda Spotify
  int total = notifs.size();
  if (total > NOTIF_ROWS)
  {
    int trackH = NOTIF_ROWS * (NOTIF_H + NOTIF_GAP) - NOTIF_GAP;
    int thumbH = max(14, trackH * NOTIF_ROWS / total);
    int thumbY = NOTIF_Y + (trackH - thumbH) * notifScroll / (total - NOTIF_ROWS);
    rrect(307, NOTIF_Y, 3, trackH, 1, C_TRACK);
    rrect(307, thumbY, 3, thumbH, 1, themeAccent);
  }
}

void drawNotifPage()
{
  drawBackdrop();
  drawTopBar("NOTIFICHE", themeAccent);
  if (!notifs.empty())
    pill(160, 5, 66, 17, "CANCELLA", themeAccent, C_PANEL2);
  drawNotifList();
}

void notifScrollBy(int dy)
{
  int rows = max(1, abs(dy) / (NOTIF_H + NOTIF_GAP));
  int maxFirst = max(0, (int)notifs.size() - NOTIF_ROWS);
  notifScroll = constrain(notifScroll + (dy < 0 ? rows : -rows), 0, maxFirst);
  drawNotifList();
}

void notifEnter()
{
  notifScroll = 0;
  notifListDirty = false;
  notifUnread = false;
  drawNotifPage();
}

void notifLoop()
{
  static bool wasLive = false;
  if (micSyncLive() != wasLive)
  {
    wasLive = !wasLive;
    if (notifs.empty())
      drawNotifList(); // aggiorna il suggerimento quando lo script si collega o si scollega
  }
  if (!notifListDirty)
    return;
  notifListDirty = false;
  notifScroll = 0;
  drawNotifPage();
  drawNavBar();
}

void notifTap(int x, int y)
{
  if (y < 28 && x >= 156 && x < 230 && !notifs.empty())
  {
    notifs.clear();
    notifScroll = 0;
    drawNotifPage();
  }
}

// Chiamato dal loop principale: mostra il popup e lo chiude dopo NOTIF_POPUP_MS.
void notifService()
{
  if (notifPopupNew)
  {
    notifPopupNew = false;
    if (curPage != NOTIF_PAGE && !notifs.empty())
    {
      drawNotifPopup(notifs[0]);
      notifPopupUntil = millis() + NOTIF_POPUP_MS;
    }
  }
  if (notifPopupUntil && timeReached(millis(), notifPopupUntil))
    switchPage(curPage); // ridisegna la pagina sotto il popup
  if (notifUnread != navDotShown)
    drawNavBar();
}

// ########==================== NAVIGAZIONE / GESTIONE PAGINE ====================########
struct Page
{
  void (*enter)();
  void (*loop)();
  void (*tap)(int, int);
};

const Page pages[] = {
    {playlistEnter, playlistLoop, playlistTap}, // 0
    {spEnter, spLoop, spTap},                   // 1
    {notifEnter, notifLoop, notifTap},          // 2 NOTIFICHE (NOTIF_PAGE)
    {weatherEnter, weatherLoop, weatherTap},    // 3
    {clockEnter, clockLoop, clockTap},          // 4 HOME
    {radarEnter, radarLoop, radarTap},          // 5
    {f1Enter, f1Loop, f1Tap},                   // 6
    {calendarEnter, calendarLoop, calendarTap}, // 7 CALENDARIO
    {pcEnter, pcLoop, pcTap}                    // 8 PC / RIUNIONE
};
constexpr int NUM_PAGES = sizeof(pages) / sizeof(pages[0]);
constexpr int HOME_PAGE = 4;
int curPage = HOME_PAGE;

int pageTabX(int i)
{
  return (2 * i + 1) * 320 / (2 * NUM_PAGES);
}

// Dock flottante in vetro: la pagina attiva ha una "pillola" tinta d'accento.
void drawNavBar()
{
  const uint16_t acc = themeAccent;
  bool sp = isSpotifyPage(curPage);
  uint16_t dock = sp ? SP_NAV : C_NAV;
  uint16_t line = sp ? SP_LINE : C_LINE;
  uint16_t inactive = sp ? SP_FAINT : C_FAINT;
  fillBg(0, NAV_Y, 320, NAV_H);
  panel(3, NAV_Y + 1, 314, NAV_H - 2, 12, dock, line);
  for (int i = 0; i < NUM_PAGES; i++)
  {
    int cx = pageTabX(i), cy = NAV_Y + 14;
    if (i == curPage)
    {
      uint16_t chip = mix565(dock, acc, 70);
      rrect(cx - 18, NAV_Y + 3, 36, 23, 10, chip, dock);
      drawTabIcon(i, cx, cy, acc, chip);
    }
    else
      drawTabIcon(i, cx, cy, inactive, dock);
  }
  if (notifUnread)
  {
    int cx = pageTabX(NOTIF_PAGE) + 8;
    tft.fillCircle(cx, NAV_Y + 6, 4, dock); // anello color dock: stacca il puntino dall'icona
    tft.fillCircle(cx, NAV_Y + 6, 3, C_RED);
  }
  navDotShown = notifUnread;
}

void switchPage(int n)
{
#ifdef WOKWI
  Serial.printf("[WOKWI] Pagina -> %d\n", (n + NUM_PAGES) % NUM_PAGES);
#endif
  curPage = (n + NUM_PAGES) % NUM_PAGES;
  notifPopupUntil = 0; // il ridisegno completo copre l'eventuale popup
  // Ogni pagina ha il suo bagliore; il player usa il colore della copertina.
  applyTheme(curPage == 1 ? SP_GREEN : pageAccent(curPage), isSpotifyPage(curPage));
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  pages[curPage].enter();
  drawNavBar();
}

// Aggiorna il meteo solo quando non si e' sulle pagine Spotify/Playlist.
// Ridisegna soltanto le pagine che mostrano il meteo: il radar o lo SPORT
// non vengono riscaricati ogni 15 minuti.
void maybeFetchWeather()
{
#ifdef WOKWI
  return;
#endif
  if (isSpotifyPage(curPage) || WiFi.status() != WL_CONNECTED)
    return;
  uint32_t now = millis();
  bool stale = weatherOk && (now - weatherTs > WEATHER_REFRESH_MS);
  bool retry = !weatherOk && (weatherTry == 0 || now - weatherTry > 60000);
  if (!stale && !retry)
    return;
  if (!fetchWeather())
    return;
  if (curPage == 3)
    switchPage(curPage);
  else if (curPage == HOME_PAGE)
    lastMin = -1; // clockLoop ridisegna tutto al prossimo giro
}

// Nessuna sospensione/screensaver automatica.
// Il display resta sulla pagina corrente finche l'utente non cambia pagina.

// ########==================== LED RGB - AVVIO E RIPRODUZIONE SPOTIFY ====================########
constexpr int RGB_PWM_CH_R = 0;
constexpr int RGB_PWM_CH_G = 1;
constexpr int RGB_PWM_CH_B = 2;
constexpr int RGB_PWM_FREQ = 5000;
constexpr int RGB_PWM_BITS = 8;

void writeRgbPwm(uint8_t red, uint8_t green, uint8_t blue)
{
  // Active-low: 255 significa spento sul pin, 0 significa piena luminosita'.
  ledcWrite(RGB_PWM_CH_R, 255 - red);
  ledcWrite(RGB_PWM_CH_G, 255 - green);
  ledcWrite(RGB_PWM_CH_B, 255 - blue);
}

void setRgbLed(uint8_t red, uint8_t green, uint8_t blue)
{
  if (!rgbLedEnabled)
  {
    writeRgbPwm(0, 0, 0);
    return;
  }

  writeRgbPwm(red, green, blue);
}

void initRgbLed()
{
  ledcSetup(RGB_PWM_CH_R, RGB_PWM_FREQ, RGB_PWM_BITS);
  ledcSetup(RGB_PWM_CH_G, RGB_PWM_FREQ, RGB_PWM_BITS);
  ledcSetup(RGB_PWM_CH_B, RGB_PWM_FREQ, RGB_PWM_BITS);

  ledcAttachPin(RGB_LED_R, RGB_PWM_CH_R);
  ledcAttachPin(RGB_LED_G, RGB_PWM_CH_G);
  ledcAttachPin(RGB_LED_B, RGB_PWM_CH_B);

  writeRgbPwm(0, 0, 0);
}

void setBootRgbLed(int active)
{
  if (!rgbLedEnabled)
  {
    writeRgbPwm(0, 0, 0);
    return;
  }

  switch (active & 3)
  {
  case 0:
    setRgbLed(30, 220, 60); // verde
    break;
  case 1:
    setRgbLed(64, 200, 255); // ciano
    break;
  case 2:
    setRgbLed(255, 210, 0); // ambra
    break;
  default:
    setRgbLed(255, 25, 25); // rosso
    break;
  }
}

void updateRgbLed()
{
  // Atmosfera piu' luminosa e stabile: pulsazione lenta (9 s per ciclo),
  // colori saturi e dissolvenza di circa 2-3 s fra le copertine.
  static uint32_t lastUpdate = 0;
  static uint8_t actualR = 0, actualG = 0, actualB = 0;
  if (!rgbLedEnabled)
  {
    actualR = actualG = actualB = 0;
    writeRgbPwm(0, 0, 0);
    return;
  }

  uint32_t now = millis();
  if (now - lastUpdate < 40)
    return;
  lastUpdate = now;

  // Notifica nuova: lampeggia col colore dell'app, poi torna dolcemente allo stato Spotify.
  if (notifLedUntil && !timeReached(now, notifLedUntil))
  {
    bool on = (now / 400) % 2 == 0;
    actualR = on ? notifLedRgb[0] : 0;
    actualG = on ? notifLedRgb[1] : 0;
    actualB = on ? notifLedRgb[2] : 0;
    writeRgbPwm(actualR, actualG, actualB);
    return;
  }

  uint8_t targetR = 0, targetG = 0, targetB = 0;
  if (T.playing)
  {
    // Un respiro completo ogni 9 s, da 60% a 100%: molto piu' lento
    // della vecchia onda (~2.2 s) ma con una variazione ben visibile.
    const uint32_t CYCLE = 9000;
    uint32_t phase = now % CYCLE;
    uint32_t wave = phase <= CYCLE / 2 ? phase : CYCLE - phase;
    uint16_t brightness = 155 + wave * 100 / (CYCLE / 2);
    targetG = brightness; // verde Spotify
  }

  // Una dissolvenza morbida; non salta di colore con i singoli campioni.
  auto approach = [](uint8_t current, uint8_t target) -> uint8_t
  {
    if (current < target)
      return (target - current > 4) ? current + 4 : target;
    return (current - target > 4) ? current - 4 : target;
  };
  actualR = approach(actualR, targetR);
  actualG = approach(actualG, targetG);
  actualB = approach(actualB, targetB);
  writeRgbPwm(actualR, actualG, actualB);
}

// ########==================== NAVIGAZIONE ALTERNATIVA SOLO WOKWI ====================########
// Pulsanti virtuali nel diagram.json: tasti freccia sinistra/destra.
// Non sono presenti ne' usati nel firmware ESP32 reale.
#ifdef WOKWI
constexpr uint8_t WOKWI_PREV_PIN = 27;
constexpr uint8_t WOKWI_NEXT_PIN = 22;

void handleWokwiNavigation()
{
  static bool prevWasDown = false, nextWasDown = false;
  static uint32_t lastPress = 0;
  bool prevDown = digitalRead(WOKWI_PREV_PIN) == LOW;
  bool nextDown = digitalRead(WOKWI_NEXT_PIN) == LOW;
  uint32_t now = millis();

  if ((uint32_t)(now - lastPress) > 140)
  {
    if (prevDown && !prevWasDown)
    {
      lastPress = now;
      switchPage(curPage - 1);
    }
    else if (nextDown && !nextWasDown)
    {
      lastPress = now;
      switchPage(curPage + 1);
    }
  }
  prevWasDown = prevDown;
  nextWasDown = nextDown;
}
#endif

// ########==================== TOUCH / GESTURE ====================########
bool tsDown = false;
uint32_t tsLastSeen = 0, tsStartT = 0;
int sx, sy, lx, ly;

void handleTouch()
{
#ifdef WOKWI
  // Diagnostica Wokwi: ci serve capire se il click arriva davvero
  // all'XPT2046 simulato prima di modificare ancora la navigazione.
  bool now = ts.touched();
#else
  bool now = ts.tirqTouched() && ts.touched();
#endif

  uint32_t ms = millis();

  if (now)
  {
    TS_Point p = ts.getPoint();

    int x = calibratedTouchX(p.x);
    int y = calibratedTouchY(p.y);

    if (!tsDown)
    {
      tsDown = true;
      sx = x;
      sy = y;
      tsStartT = ms;
#ifdef WOKWI
      Serial.printf("[WOKWI TOUCH] down raw=(%d,%d) mapped=(%d,%d)\n", p.x, p.y, x, y);
#endif
    }
    lx = x;
    ly = y;
    tsLastSeen = ms;
  }
  else if (tsDown && ms - tsLastSeen > 60)
  {
    tsDown = false;
    int dx = lx - sx, dy = ly - sy;
    bool tap = abs(dx) < 20 && abs(dy) < 20 && ms - tsStartT < 800;
#ifdef WOKWI
    Serial.printf("[WOKWI TOUCH] up mapped=(%d,%d) dx=%d dy=%d tap=%d\n",
                  lx, ly, dx, dy, (int)tap);
#endif

    if (tap && notifPopupUntil && ly < NAV_Y - 2)
    {
      // Tocco sul popup: apre lo storico; altrove lo chiude.
      bool inside = lx >= POPUP_X && lx < POPUP_X + POPUP_W && ly >= POPUP_Y && ly < POPUP_Y + POPUP_H;
      switchPage(inside ? NOTIF_PAGE : curPage);
    }
    else if (tap && ly >= NAV_Y - 2)
    {
      // Tab bar: la colonna toccata decide la pagina.
      int best = constrain(lx * NUM_PAGES / 320, 0, NUM_PAGES - 1);
      if (best != curPage)
        switchPage(best);
    }
    else if (curPage == 0 && spotifyLibraryView == LIB_QUEUE &&
             abs(dy) > 24 && abs(dy) > abs(dx))
    {
      int visible = min(QUEUE_VISIBLE_ROWS, (int)spotifyQueue.size());
      int maxFirst = max(0, (int)spotifyQueue.size() - visible);
      int rows = max(1, abs(dy) / (Q_H + Q_GY));
      if (dy < 0)
        queueScrollRow = min(maxFirst, queueScrollRow + rows);
      else
        queueScrollRow = max(0, queueScrollRow - rows);
      drawPlaylistPage();
    }
    else if (curPage == NOTIF_PAGE && abs(dy) > 24 && abs(dy) > abs(dx))
    {
      notifScrollBy(dy);
    }
    else if (abs(dx) > 50 && abs(dx) > abs(dy) * 2)
    {
      switchPage(curPage + (dx < 0 ? 1 : -1));
    }
    else if (tap)
    {
      pages[curPage].tap(lx, ly);
    }
  }
}

// ########==================== BOOT ANIMATION ====================########

static const uint16_t BOOT_DOTS[4] = {C_GREEN, C_CYAN, C_AMBER, C_RED};

void drawBootLogo(float phase)
{
  // marchio: quattro nodi (le sezioni dell'hub) attorno a un centro
  int active = ((int)(phase * 2.5f)) % 4;
  setBootRgbLed(active);
  const int cx = 160, cy = 84;
  panel(cx - 36, cy - 36, 72, 72, 20, C_PANEL, mix565(C_LINE, BOOT_DOTS[active], 90));
  static const int8_t ox[4] = {-16, 16, 16, -16}, oy[4] = {-16, -16, 16, 16};
  tft.drawLine(cx + ox[0], cy + oy[0], cx + ox[2], cy + oy[2], C_LINE);
  tft.drawLine(cx + ox[1], cy + oy[1], cx + ox[3], cy + oy[3], C_LINE);
  for (int i = 0; i < 4; i++)
  {
    if (i == active)
      glowDisc(cx + ox[i], cy + oy[i], 7, BOOT_DOTS[i], 4, C_PANEL, 160);
    else
    {
      // ripulisce l'alone del nodo precedente
      disc(cx + ox[i], cy + oy[i], 11, C_PANEL, C_PANEL);
      disc(cx + ox[i], cy + oy[i], 6, mix565(C_PANEL, BOOT_DOTS[i], 110), C_PANEL);
    }
  }
  glowDisc(cx, cy, 4, C_FG, 2, C_PANEL, 120);
}

void drawBootProgress(float progress)
{
  progress = constrain(progress, 0.0f, 1.0f);
  constexpr int X = 80, Y = 198, W = 160, H = 4;
  rrect(X, Y, W, H, 2, C_TRACK);
  int fillW = (int)(W * progress);
  if (fillW > 0)
  {
    tft.fillRectHGradient(X, Y, max(fillW, H), H, C_GREEN, mix565(C_GREEN, C_CYAN, (uint8_t)(255 * progress)));
    disc(X + max(fillW, H), Y + 2, 2, C_FG);
  }
}

void drawBootFrame(
    float phase,
    float progress,
    const char *status = nullptr)
{
  if (!bootStatic)
  {
    applyTheme(C_CYAN, false);
    drawBackdrop();
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_FG);
    tft.drawString("CYD HUB", 160, 144, 4);
    pill(140, 160, 40, 13, HUB_VERSION, C_BG, C_CYAN, 1);
    bootStatic = true;
  }
  drawBootLogo(phase);
  fillBg(30, 175, 260, 16);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM);
  tft.drawString(status && strlen(status) ? status : "avvio...", 160, 183, 2);
  drawBootProgress(progress);
  tft.setTextDatum(TL_DATUM);
}

void bootIntro()
{
  bootStatic = false;
  for (int i = 0; i < 8; i++)
  {
    drawBootFrame(i * 0.4f, 0.05f + i * 0.02f, "avvio...");
    delay(90);
  }
}
// ########==================== TEST CALIBRAZIONE COLORI ====================########

uint16_t testColor565(uint8_t r, uint8_t g, uint8_t b)
{
  return C565(r, g, b);
}

void showColorCalibrationTest()
{
  tft.fillScreen(TFT_BLACK);

  const int cellW = 106;
  const int cellH = 70;

  struct TestColor
  {
    const char *name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };

  const TestColor colors[9] = {
      {"ROSSO", 255, 0, 0},
      {"VERDE", 0, 255, 0},
      {"BLU", 0, 0, 255},

      {"GIALLO", 255, 255, 0},
      {"CIANO", 0, 255, 255},
      {"MAGENTA", 255, 0, 255},

      {"BIANCO", 255, 255, 255},
      {"GRIGIO", 128, 128, 128},
      {"NERO", 0, 0, 0}};

  tft.setTextDatum(MC_DATUM);

  for (int i = 0; i < 9; i++)
  {
    int col = i % 3;
    int row = i / 3;

    int x = col * cellW;
    int y = row * cellH;

    uint16_t c = testColor565(
        colors[i].r,
        colors[i].g,
        colors[i].b);

    tft.fillRect(x, y, cellW, cellH, c);

    // Scritta nera sui colori chiari, bianca sugli altri
    bool light =
        (colors[i].r + colors[i].g + colors[i].b) > 380;

    tft.setTextColor(light ? TFT_BLACK : TFT_WHITE);
    tft.drawString(
        colors[i].name,
        x + cellW / 2,
        y + cellH / 2,
        2);
  }

  tft.setTextDatum(TL_DATUM);
}


// ########==================== SETUP / LOOP PRINCIPALE ====================########
bool connectWiFi(const char *ssid, const char *password, int32_t channel, uint32_t timeoutMs)
{
  if (!ssid || !*ssid)
    return false;
  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(ssid, password, channel);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < timeoutMs)
  {
    delay(100);
  }

  return WiFi.status() == WL_CONNECTED;
}

void setup()
{
  Serial.begin(115200);

  // Preferenze caricate prima del boot per rispettare LED ON/OFF.
  prefs.begin("cyddash", false);
  rgbLedEnabled = prefs.getBool("led_on", true);
  bleEnabled = prefs.getBool("ble_on", true);
  notifEnabled = prefs.getBool("notif_on", true);
  initRgbLed();

  // Display
  tft.init();
  tft.setRotation(ROTATION);
  tft.invertDisplay(DISPLAY_INVERT);
  tft.setSwapBytes(true);
  tft.fillScreen(C_BG);
  // showColorCalibrationTest();
  // delay(10000);
  // Breve intro animata.
  bootIntro();

  // Controlli alternativi di navigazione presenti solo in Wokwi.
#ifdef WOKWI
  pinMode(WOKWI_PREV_PIN, INPUT_PULLUP);
  pinMode(WOKWI_NEXT_PIN, INPUT_PULLUP);
#endif

  // Touch
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(ROTATION);

  // Carica calibrazione e gestisci reset Spotify tenendo premuto all'avvio.
  loadTouchCalibration();
#ifndef WOKWI
  if (ts.tirqTouched() && ts.touched())
  {
    prefs.remove("rt");
    centerMsg("Account Spotify", "dimenticato, rilascia lo schermo");
    delay(2000);
  }
#endif
  refreshToken = prefs.getString("rt", "");

  // Bluetooth avviato subito: il controllo del PC funziona anche senza WiFi.
  bleInit();

#ifdef WOKWI
  // In simulazione NON si usano credenziali, Internet o servizi esterni.
  // Timestamp fisso per far funzionare orologio e calendario senza NTP.
  timeval demoTime = {1791806400, 0};
  settimeofday(&demoTime, nullptr);
  setenv("TZ", TZ_INFO, 1);
  tzset();
  W.t = 21;
  W.feels = 20;
  W.wind = 8;
  W.hum = 61;
  W.code = 2;
  W.isDay = 1;
  for (int i = 0; i < 4; i++)
  {
    W.tmax[i] = 23 - i;
    W.tmin[i] = 14 - i;
    W.dcode[i] = i % 3;
    W.wday[i] = (1 + i) % 7;
  }
  weatherOk = true;
  weatherTs = millis();
  initFilter();
  T.playing = true;
  T.key = "";
  switchPage(HOME_PAGE);
  updateRgbLed();
  Serial.println("WOKWI DEMO: 8 pagine, tabs, swipe e coda pronti");
  Serial.println("WOKWI NAV: frecce SX/DX o pulsanti PREV/NEXT nel diagramma");
#else

  // ---------------------------------------------------------
  // WiFi
  // ---------------------------------------------------------
  drawBootFrame(
      0.8f,
      0.15f,
      (String("connecting to ") + WIFI_SSID_PRIMARY).c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  bool wifiOk = connectWiFi(WIFI_SSID_PRIMARY, WIFI_PASS_PRIMARY, WIFI_CHANNEL, 15000);
  if (!wifiOk)
  {
    // Rete principale non raggiungibile: prova quella di backup definita in secret.h.
    drawBootFrame(
        1.0f,
        0.22f,
        (String("connecting to ") + WIFI_SSID_2).c_str());
    wifiOk = connectWiFi(WIFI_SSID_2, WIFI_PASS_2, 0, 15000);
  }

  drawBootFrame(
      1.2f,
      0.30f,
      wifiOk ? "WiFi connected" : "WiFi offline");

  delay(180);

  // ---------------------------------------------------------
  // Ora / NTP
  // ---------------------------------------------------------
  drawBootFrame(
      1.6f,
      0.40f,
      "sync time");

  configTzTime(
      TZ_INFO,
      "pool.ntp.org",
      "time.google.com");

  struct tm ti;
  bool timeOk = getLocalTime(&ti, 10000);

  drawBootFrame(
      2.0f,
      0.50f,
      timeOk ? "time synced" : "time offline");

  delay(150);

  // ---------------------------------------------------------
  // Meteo
  // ---------------------------------------------------------
  drawBootFrame(
      2.4f,
      0.60f,
      "weather");

  bool bootWeatherOk = fetchWeather();

  drawBootFrame(
      2.8f,
      0.70f,
      bootWeatherOk ? "weather ready" : "weather offline");

  delay(160);

  // ---------------------------------------------------------
  // Spotify
  // ---------------------------------------------------------
  drawBootFrame(
      3.2f,
      0.80f,
      "spotify");

  initFilter();

#if REQUIRE_SPOTIFY_AUTH
  // Senza WiFi la pagina di collegamento non sarebbe raggiungibile: si salta e
  // le pagine Spotify restano semplicemente vuote fino al prossimo avvio.
  if (!refreshToken.length() && WiFi.status() == WL_CONNECTED)
  {
    drawBootFrame(
        3.6f,
        0.85f,
        "Spotify login");

    delay(150);
    runAuthMode();
  }
#else
  Serial.println("Wokwi: autenticazione Spotify saltata");
#endif

  // ---------------------------------------------------------
  // Fine boot
  // ---------------------------------------------------------
  drawBootFrame(
      4.0f,
      1.0f,
      "ready");

  delay(300);

  switchPage(HOME_PAGE);
  updateRgbLed();
#endif // WOKWI: il blocco Wi-Fi reale non va compilato nel simulatore
}

void loop()
{
#ifdef WOKWI
  handleWokwiNavigation();
#endif
  handleTouch();
  bleSyncService();
  notifService();

  // Con il popup a schermo la pagina non si aggiorna, per non disegnarci sopra.
  if (!notifPopupUntil)
    pages[curPage].loop();

  updateTopBarClock();

  maybeFetchWeather();

  spotifyBackgroundPoll();

  updateRgbLed();
  delay(2);
}