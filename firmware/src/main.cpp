// Guest Wi-Fi display: receives SSID, password and QR payload via HTTP and shows them on the e-paper.
//
// POST /guest-wifi  with  Authorization: Bearer <DISPLAY_TOKEN>
//                         {"ssid": "...", "password": "...", "payload": "WIFI:..."}
// GET  /            status (without the password)
//
// The last data is kept in NVS and drawn again after a restart and once a day.
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WebServer.h>
#include <WiFi.h>
#include <qrcode.h>

#include "epd.h"
#include "secrets.h"

constexpr uint16_t BLACK = 0;
constexpr uint16_t WHITE = 1;
constexpr int MARGIN = 12;

struct GuestWifi {
  String ssid, password, payload;
};

GFXcanvas1 canvas(epd::WIDTH, epd::HEIGHT);
U8G2_FOR_ADAFRUIT_GFX u8g2;
WebServer server(80);
Preferences prefs;
GuestWifi current;
bool waitingScreenShown = false;

// Draws text in a column, breaking lines at spaces and after hyphens (or within a word that is
// too long). Returns the baseline of the last line.
int drawWrapped(const String &text, int x, int y, int width, int lineHeight) {
  String line;
  int cut = -1, resume = -1;  // end of the line and start of the next one at the last break point
  for (size_t i = 0; i < text.length();) {
    size_t n = 1;  // bytes of this UTF-8 character
    while (i + n < text.length() && (text[i + n] & 0xC0) == 0x80) n++;
    String ch = text.substring(i, i + n);
    i += n;
    if (line.length() > 0 && u8g2.getUTF8Width((line + ch).c_str()) > width) {
      String rest;
      if (cut > 0) {
        rest = line.substring(resume);
        line = line.substring(0, cut);
      }
      u8g2.drawUTF8(x, y, line.c_str());
      y += lineHeight;
      line = rest;
      cut = -1;
      if (ch == " ") continue;
    }
    line += ch;
    if (ch == " ") {
      cut = line.length() - 1;
      resume = line.length();
    } else if (ch == "-") {
      cut = resume = line.length();
    }
  }
  u8g2.drawUTF8(x, y, line.c_str());
  return y;
}

// True if no word is wider than width with the current font (words end at spaces and hyphens)
bool wordsFit(const String &text, int width) {
  int start = 0;
  for (int i = 0; i <= (int)text.length(); i++) {
    if (i < (int)text.length() && text[i] != ' ' && text[i] != '-') continue;
    int end = (i < (int)text.length() && text[i] == '-') ? i + 1 : i;
    if (u8g2.getUTF8Width(text.substring(start, end).c_str()) > width) return false;
    start = i + 1;
  }
  return true;
}

// Draws the QR code into a square of at most maxSize pixels. Returns the size used, 0 on error.
int drawQr(const String &payload, int x, int y, int maxSize) {
  // The library does not check the capacity, so pick the version here.
  // Byte mode capacity at ECC_MEDIUM for versions 1-10.
  const int capacity[] = {14, 26, 42, 62, 84, 106, 122, 152, 180, 213};
  int version = 1;
  while (version <= 10 && (int)payload.length() > capacity[version - 1]) version++;
  if (version > 10) return 0;

  QRCode qr;
  static uint8_t buffer[512];  // qrcode_getBufferSize(10) = 407
  if (qrcode_initText(&qr, buffer, version, ECC_MEDIUM, payload.c_str()) != 0) return 0;

  int scale = maxSize / qr.size;
  for (int qy = 0; qy < qr.size; qy++) {
    for (int qx = 0; qx < qr.size; qx++) {
      if (qrcode_getModule(&qr, qx, qy)) canvas.fillRect(x + qx * scale, y + qy * scale, scale, scale, BLACK);
    }
  }
  return qr.size * scale;
}

void drawPassword(const String &password, int y) {
  // Monospaced fonts with distinct 0/O and 1/l/I, the largest one that fits in one line
  const uint8_t *fonts[] = {u8g2_font_inb24_mf, u8g2_font_inb21_mf, u8g2_font_inb19_mf, u8g2_font_inb16_mf};
  int width = epd::WIDTH - 2 * MARGIN;
  for (const uint8_t *font : fonts) {
    u8g2.setFont(font);
    if (u8g2.getUTF8Width(password.c_str()) <= width) {
      u8g2.drawUTF8(MARGIN, y, password.c_str());
      return;
    }
  }
  drawWrapped(password, MARGIN, y - 20, width, 20);
}

bool render(const GuestWifi &w) {
  canvas.fillScreen(WHITE);

  u8g2.setFont(u8g2_font_helvB18_tf);
  u8g2.drawUTF8(MARGIN, 32, "Gäste-WLAN");

  int qrSize = drawQr(w.payload, MARGIN, 48, 192);

  int col = MARGIN + qrSize + 16;
  int colWidth = epd::WIDTH - MARGIN - col;
  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.drawUTF8(col, 64, "Netzwerk");
  // The largest font in which no word of the SSID has to be split
  const uint8_t *ssidFonts[] = {u8g2_font_helvB18_tf, u8g2_font_helvB14_tf, u8g2_font_helvB12_tf};
  for (const uint8_t *font : ssidFonts) {
    u8g2.setFont(font);
    if (wordsFit(w.ssid, colWidth)) break;
  }
  int y = drawWrapped(w.ssid, col, 90, colWidth, u8g2.getFontAscent() - u8g2.getFontDescent() + 4);
  u8g2.setFont(u8g2_font_helvR12_tf);
  drawWrapped("QR-Code mit der Kamera scannen oder Passwort eingeben", col, y + 36, colWidth, 17);

  // The password first: the monospaced font clears its whole glyph box, including the label above
  drawPassword(w.password, 286);
  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.drawUTF8(MARGIN, 256, "Passwort");

  return epd::show(canvas.getBuffer());
}

bool renderWaiting() {
  canvas.fillScreen(WHITE);
  u8g2.setFont(u8g2_font_helvB18_tf);
  u8g2.drawUTF8(MARGIN, 32, "Gäste-WLAN");
  u8g2.setFont(u8g2_font_helvR12_tf);
  String info = "Warte auf Daten von send_to_display.py an http://" + String(HOSTNAME) + "/ (" +
                WiFi.localIP().toString() + ")";
  drawWrapped(info, MARGIN, 70, epd::WIDTH - 2 * MARGIN, 18);
  return epd::show(canvas.getBuffer());
}

void load() {
  prefs.begin("guestwifi", true);
  current = {prefs.getString("ssid"), prefs.getString("password"), prefs.getString("payload")};
  prefs.end();
}

void save(const GuestWifi &w) {
  prefs.begin("guestwifi", false);
  prefs.putString("ssid", w.ssid);
  prefs.putString("password", w.password);
  prefs.putString("payload", w.payload);
  prefs.end();
}

void handleUpdate() {
  if (server.header("Authorization") != "Bearer " DISPLAY_TOKEN) {
    server.send(401, "text/plain", "unauthorized\n");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "invalid JSON\n");
    return;
  }
  GuestWifi w = {doc["ssid"] | "", doc["password"] | "", doc["payload"] | ""};
  if (w.ssid.isEmpty() || w.ssid.length() > 32 || w.password.length() < 8 || w.password.length() > 63 ||
      !w.payload.startsWith("WIFI:") || w.payload.length() > 200) {
    server.send(400, "text/plain", "expected ssid, password (8-63 characters) and a WIFI: payload\n");
    return;
  }
  if (w.ssid == current.ssid && w.password == current.password && w.payload == current.payload) {
    server.send(200, "text/plain", "unchanged\n");
    return;
  }
  save(w);
  current = w;
  if (!render(w)) {
    server.send(500, "text/plain", "saved, but the display did not finish the refresh\n");
    return;
  }
  server.send(200, "text/plain", "updated\n");
}

void handleStatus() {
  String ssid = current.ssid.isEmpty() ? "(none)" : current.ssid;
  server.send(200, "text/plain", "guest-display, showing SSID: " + ssid + "\n");
}

void setup() {
  Serial.begin(115200);
  epd::begin();
  u8g2.begin(canvas);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(BLACK);
  u8g2.setBackgroundColor(WHITE);

  // Draw the stored data right away, without waiting for Wi-Fi
  load();
  if (!current.ssid.isEmpty()) render(current);

  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);  // before mode(), otherwise the default name is used
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("Wi-Fi disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const char *headers[] = {"Authorization"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, handleStatus);
  server.on("/guest-wifi", HTTP_POST, handleUpdate);
  server.begin();
}

void loop() {
  static bool wasConnected = false;
  static uint32_t lastAttempt = 0;
  static uint32_t lastRefresh = 0;
  // Once a day: white, then the image again, so the panel does not burn in
  if (!current.ssid.isEmpty() && millis() - lastRefresh > 24UL * 60 * 60 * 1000) {
    lastRefresh = millis();
    Serial.println("Daily refresh");
    canvas.fillScreen(WHITE);
    epd::show(canvas.getBuffer());
    render(current);
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("Wi-Fi connected, http://%s/ (%s)\n", HOSTNAME, WiFi.localIP().toString().c_str());
    if (current.ssid.isEmpty() && !waitingScreenShown) waitingScreenShown = renderWaiting();
  }
  // Auto-reconnect does not retry when the first connection fails, so start over now and then
  if (!connected && millis() - lastAttempt > 30000) {
    lastAttempt = millis();
    Serial.printf("Wi-Fi not connected (status %d), trying again\n", WiFi.status());
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  wasConnected = connected;
  server.handleClient();
  delay(2);
}
