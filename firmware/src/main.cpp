// Guest Wi-Fi display: gets SSID, password and QR payload via MQTT and shows them on the e-paper.
//
// Subscribes to MQTT_TOPIC (retained, published by publish_mqtt.py):
//   {"ssid": "...", "password": "...", "payload": "WIFI:...", "updated": "..."}
// Subscribes to MQTT_IMAGE_TOPIC (retained, published by send_image.py): a 400x300 1-bit bitmap,
// 15000 bytes, MSB first, 1 = white. "clear" or an empty message removes it.
// HOME switches between the image and the guest Wi-Fi. New data of the other screen is only saved.
// Publishes its state to MQTT_STATUS_TOPIC (retained, without the password).
//
// With SLEEP_INTERVAL_S in secrets.h the display runs on a battery: it wakes up every
// SLEEP_INTERVAL_S seconds, reads the retained message, redraws if it changed, publishes its status
// and goes back to deep sleep. Any key wakes it up too; it stays awake while the debug screen is open.
//
// The last data is kept in NVS, the image in LittleFS; both are drawn again after a restart and once a day.
// Rotary switch: turn for the debug screens, press to rotate the image by 180 degrees.
// EXIT: back from the debug screens. HOME: back as well, otherwise switch between image and guest Wi-Fi.
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <qrcode.h>
#include <time.h>

#include "epd.h"
#include "secrets.h"

// Optional: seconds between two checks in battery mode, 0 keeps the display connected all the time
#ifndef SLEEP_INTERVAL_S
#define SLEEP_INTERVAL_S 0
#endif
constexpr bool SLEEP_MODE = SLEEP_INTERVAL_S > 0;

// secrets.h from before images were supported has no MQTT_IMAGE_TOPIC
#ifndef MQTT_IMAGE_TOPIC
#define MQTT_IMAGE_TOPIC "guestpass/image"
#endif
constexpr size_t IMAGE_SIZE = epd::WIDTH * epd::HEIGHT / 8;
const char *IMAGE_FILE = "/image.bin";

constexpr uint16_t BLACK = 0;
constexpr uint16_t WHITE = 1;
constexpr int MARGIN = 12;

// Keys, active low
constexpr int KEY_EXIT = 1;
constexpr int KEY_HOME = 2;
constexpr int KEY_DOWN = 4;
constexpr int KEY_OK = 5;
constexpr int KEY_UP = 6;
constexpr int KEYS[] = {KEY_EXIT, KEY_HOME, KEY_DOWN, KEY_OK, KEY_UP};

constexpr int DEBUG_PAGES = 2;
constexpr uint32_t DEBUG_TIMEOUT = 2 * 60 * 1000;
constexpr int LOG_SIZE = 14;
constexpr uint32_t CYCLE_TIMEOUT = 40 * 1000;  // battery mode: give up waiting for Wi-Fi or the broker
constexpr uint32_t IDLE_BEFORE_SLEEP = 5 * 1000;  // battery mode: stay awake this long after the last activity

struct GuestWifi {
  String ssid, password, payload, updated;
};

struct Event {
  time_t time;
  uint32_t uptime;
  String text;
};

GFXcanvas1 canvas(epd::WIDTH, epd::HEIGHT);
U8G2_FOR_ADAFRUIT_GFX u8g2;
WiFiClient net;
PubSubClient mqtt(net);
Preferences prefs;
GuestWifi current;
uint8_t image[IMAGE_SIZE];
bool hasImage = false;
bool imageView = false;  // the image is the main screen instead of the guest Wi-Fi, kept in NVS
bool waitingScreenShown = false;
bool flipped = false;  // image rotated by 180 degrees, kept in NVS
int debugPage = -1;  // -1: the guest Wi-Fi screen
uint32_t debugSince = 0;
Event events[LOG_SIZE];  // newest first
int eventCount = 0;
Event lastMessage = {0, 0, ""};
int wifiDrops = 0, mqttDrops = 0;
uint32_t lastActivity = 0;  // battery mode: last key press or end of the debug screen
bool gotMessage = false;  // battery mode: the retained message has arrived
esp_sleep_wakeup_cause_t wakeCause;
RTC_DATA_ATTR uint32_t secondsSinceRefresh = 0;  // battery mode: survives deep sleep
volatile int disconnectReason = -1;  // set by the Wi-Fi event task, logged in loop()

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
  String info = "Warte auf Daten über MQTT (" MQTT_TOPIC " auf " MQTT_HOST "). Drehrad drehen: Debug-Anzeige";
  drawWrapped(info, MARGIN, 70, epd::WIDTH - 2 * MARGIN, 18);
  return epd::show(canvas.getBuffer());
}

bool renderImage() {
  // drawBitmap honours the canvas rotation, so the 180 degree setting applies to the image too
  canvas.drawBitmap(0, 0, image, epd::WIDTH, epd::HEIGHT, WHITE, BLACK);
  return epd::show(canvas.getBuffer());
}

bool showingImage() { return hasImage && (imageView || current.ssid.isEmpty()); }

bool hasContent() { return hasImage || !current.ssid.isEmpty(); }

bool showCurrent() {
  if (showingImage()) return renderImage();
  return current.ssid.isEmpty() ? renderWaiting() : render(current);
}

bool validTime(time_t t) { return t > 1700000000; }

String duration(uint32_t s) {
  char buf[24];
  snprintf(buf, sizeof buf, "%lu d %02lu:%02lu:%02lu", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
  return buf;
}

// Wall clock if NTP has synced, uptime otherwise
String eventTime(const Event &e) {
  if (!validTime(e.time)) return "+" + duration(e.uptime);
  struct tm tm;
  localtime_r(&e.time, &tm);
  char buf[20];
  strftime(buf, sizeof buf, "%d.%m. %H:%M:%S", &tm);
  return buf;
}

void logEvent(const String &text) {
  Serial.println(text);
  for (int i = LOG_SIZE - 1; i > 0; i--) events[i] = events[i - 1];
  events[0] = {time(nullptr), millis() / 1000, text};
  if (eventCount < LOG_SIZE) eventCount++;
}

String resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Einschalten";
    case ESP_RST_SW: return "Software";
    case ESP_RST_PANIC: return "Absturz";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_BROWNOUT: return "Unterspannung";
    case ESP_RST_EXT: return "Reset-Taste";
    default: return "Code " + String(esp_reset_reason());
  }
}

bool renderDebug(int page) {
  canvas.fillScreen(WHITE);
  u8g2.setFont(u8g2_font_helvB14_tf);
  u8g2.drawUTF8(MARGIN, 24, page == 0 ? "Debug: Status" : "Debug: Ereignisse");
  String pageNo = String(page + 1) + "/" + String(DEBUG_PAGES);
  u8g2.drawUTF8(epd::WIDTH - MARGIN - u8g2.getUTF8Width(pageNo.c_str()), 24, pageNo.c_str());

  u8g2.setFont(u8g2_font_helvR10_tf);
  int y = 48;
  auto line = [&](const String &label, const String &value, int col) {
    u8g2.drawUTF8(MARGIN, y, label.c_str());
    u8g2.drawUTF8(MARGIN + col, y, value.c_str());
    y += 16;
  };
  if (page == 0) {
    const int col = 120;
    bool wifi = WiFi.status() == WL_CONNECTED;
    Event now = {time(nullptr), millis() / 1000, ""};
    line("Firmware", __DATE__ " " __TIME__, col);
    line("Laufzeit", duration(now.uptime), col);
    line("Letzter Neustart", resetReason(), col);
    line("Uhrzeit", validTime(now.time) ? eventTime(now) : "noch keine (NTP)", col);
    line("WLAN", wifi ? WiFi.SSID() + ", " + String(WiFi.RSSI()) + " dBm" : "nicht verbunden", col);
    line("IP", wifi ? WiFi.localIP().toString() : "-", col);
    line("MAC", WiFi.macAddress(), col);
    line("MQTT", String(MQTT_HOST) + ":" + String(MQTT_PORT) +
                     (mqtt.connected() ? ", verbunden" : ", Fehler " + String(mqtt.state())), col);
    line("Topic", MQTT_TOPIC, col);
    line("Letzte Nachricht", lastMessage.uptime ? eventTime(lastMessage) : "noch keine", col);
    line("Gäste-SSID", current.ssid.isEmpty() ? "(keine)" : current.ssid, col);
    line("Stand der Daten", current.updated.isEmpty() ? "-" : current.updated, col);
    line("Bild", !hasImage ? "keins" : showingImage() ? "wird angezeigt" : "vorhanden, HOME zeigt es", col);
    line("Abbrüche", String(wifiDrops) + " WLAN, " + String(mqttDrops) + " MQTT", col);
  } else {
    for (int i = 0; i < eventCount; i++) line(eventTime(events[i]), events[i].text, 120);
    if (eventCount == 0) line("Noch keine Ereignisse", "", 0);
  }

  canvas.drawFastHLine(MARGIN, 274, epd::WIDTH - 2 * MARGIN, BLACK);
  u8g2.drawUTF8(MARGIN, 292, "Drehen: blättern   Drücken: 180° drehen   EXIT: zurück");
  return epd::show(canvas.getBuffer());
}

void load() {
  prefs.begin("guestwifi", true);
  current = {prefs.getString("ssid"), prefs.getString("password"), prefs.getString("payload"),
             prefs.getString("updated")};
  flipped = prefs.getBool("flipped");
  imageView = prefs.getBool("imageView");
  prefs.end();
  hasImage = LittleFS.exists(IMAGE_FILE);
  if (hasImage) {
    File f = LittleFS.open(IMAGE_FILE, "r");
    hasImage = f.read(image, IMAGE_SIZE) == IMAGE_SIZE;
    f.close();
  }
}

void save(const GuestWifi &w) {
  prefs.begin("guestwifi", false);
  prefs.putString("ssid", w.ssid);
  prefs.putString("password", w.password);
  prefs.putString("payload", w.payload);
  prefs.putString("updated", w.updated);
  prefs.end();
}

void setImageView(bool on) {
  if (imageView == on) return;
  imageView = on;
  prefs.begin("guestwifi", false);
  prefs.putBool("imageView", on);
  prefs.end();
}

void saveImage() {
  if (!hasImage) {
    LittleFS.remove(IMAGE_FILE);
    return;
  }
  File f = LittleFS.open(IMAGE_FILE, "w");
  if (!f || f.write(image, IMAGE_SIZE) != IMAGE_SIZE) logEvent("Bild nicht gespeichert");
  f.close();
}

void publishStatus(bool online = true) {
  JsonDocument doc;
  doc["online"] = online;
  if (SLEEP_MODE) doc["sleep_interval"] = SLEEP_INTERVAL_S;
  doc["shown_ssid"] = current.ssid;
  doc["updated"] = current.updated;
  doc["image"] = hasImage;
  doc["showing"] = showingImage() ? "image" : "wifi";
  doc["rssi"] = WiFi.RSSI();
  doc["ip"] = WiFi.localIP().toString();
  doc["uptime"] = millis() / 1000;
  doc["wifi_drops"] = wifiDrops;
  doc["mqtt_drops"] = mqttDrops;
  doc["firmware"] = __DATE__ " " __TIME__;
  String out;
  serializeJson(doc, out);
  mqtt.publish(MQTT_STATUS_TOPIC, out.c_str(), true);
}

void onImage(const byte *payload, unsigned int length) {
  bool wasShowing = showingImage();
  if (length == 0 || (length == 5 && memcmp(payload, "clear", 5) == 0)) {
    if (!hasImage) return;
    hasImage = false;
    saveImage();
    setImageView(false);
    logEvent("Bild gelöscht");
  } else if (length != IMAGE_SIZE) {
    logEvent("Bild: " + String(length) + " statt " + String(IMAGE_SIZE) + " Bytes");
    return;
  } else if (hasImage && memcmp(image, payload, IMAGE_SIZE) == 0) {
    logEvent("Bild unverändert");
    return;
  } else {
    memcpy(image, payload, IMAGE_SIZE);
    hasImage = true;
    saveImage();
    logEvent("Neues Bild");
  }
  // Redraw only if the image is (or was) on screen, the debug screen draws it when it closes
  if (debugPage < 0 && (wasShowing || showingImage()) && !showCurrent()) logEvent("Refresh nicht fertig");
  publishStatus();
}

void onMessage(char *topic, byte *payload, unsigned int length) {
  gotMessage = true;
  lastMessage = {time(nullptr), max(millis() / 1000, 1UL), ""};
  if (strcmp(topic, MQTT_IMAGE_TOPIC) == 0) {
    onImage(payload, length);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) {
    logEvent("MQTT: ungültiges JSON");
    return;
  }
  GuestWifi w = {doc["ssid"] | "", doc["password"] | "", doc["payload"] | "", doc["updated"] | ""};
  if (w.ssid.isEmpty() || w.ssid.length() > 32 || w.password.length() < 8 || w.password.length() > 63 ||
      !w.payload.startsWith("WIFI:") || w.payload.length() > 200) {
    logEvent("MQTT: Daten unvollständig");
    return;
  }
  if (w.ssid == current.ssid && w.password == current.password && w.payload == current.payload) {
    if (w.updated != current.updated) {
      current.updated = w.updated;
      save(current);
    }
    logEvent("Daten unverändert");
    return;
  }
  save(w);
  current = w;
  logEvent("Neue Daten: " + w.ssid);
  // In image view the data is only saved. The debug screen stays until it is closed, then the new data is drawn.
  if (debugPage < 0 && !showingImage() && !render(w)) logEvent("Refresh nicht fertig");
  publishStatus();
}

void connectMqtt() {
  const char *user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  if (!mqtt.connect(HOSTNAME, user, MQTT_PASSWORD, MQTT_STATUS_TOPIC, 0, true, "{\"online\":false}")) {
    logEvent("MQTT-Fehler " + String(mqtt.state()));
    return;
  }
  logEvent("MQTT verbunden");
  // The retained messages arrive right away
  mqtt.subscribe(MQTT_TOPIC, 1);
  mqtt.subscribe(MQTT_IMAGE_TOPIC, 1);
  publishStatus();
}

// Returns the key that was just pressed, or -1
int pressedKey() {
  static bool down[5];
  static uint32_t lastChange[5];
  for (int i = 0; i < 5; i++) {
    bool d = digitalRead(KEYS[i]) == LOW;
    if (d != down[i] && millis() - lastChange[i] > 30) {
      down[i] = d;
      lastChange[i] = millis();
      if (d) return KEYS[i];
    }
  }
  return -1;
}

void handleKey(int key) {
  if (key == KEY_EXIT || key == KEY_HOME) {
    if (debugPage >= 0) {
      debugPage = -1;
    } else if (key == KEY_HOME && hasImage && !current.ssid.isEmpty()) {
      setImageView(!showingImage());
    } else {
      return;
    }
    showCurrent();
    return;
  }
  if (key == KEY_OK) {
    flipped = !flipped;
    prefs.begin("guestwifi", false);
    prefs.putBool("flipped", flipped);
    prefs.end();
    canvas.setRotation(flipped ? 2 : 0);
    logEvent(flipped ? "Bild um 180° gedreht" : "Bild normal ausgerichtet");
    if (debugPage < 0) {
      showCurrent();
      return;
    }
  } else if (debugPage < 0) {
    debugPage = 0;
  } else if (key == KEY_UP) {
    debugPage = (debugPage + DEBUG_PAGES - 1) % DEBUG_PAGES;
  } else if (key == KEY_DOWN) {
    debugPage = (debugPage + 1) % DEBUG_PAGES;
  }
  debugSince = millis();
  renderDebug(debugPage);
}

// White, then the image again, so the panel does not burn in
void dailyRefresh() {
  logEvent("Täglicher Refresh");
  canvas.fillScreen(WHITE);
  epd::show(canvas.getBuffer());
  showCurrent();
}

bool anyKeyDown() {
  for (int key : KEYS) {
    if (digitalRead(key) == LOW) return true;
  }
  return false;
}

[[noreturn]] void sleepUntilNextCheck() {
  if (mqtt.connected()) {
    publishStatus(false);  // no last will on a clean disconnect, so say it here
    mqtt.loop();
    mqtt.disconnect();
  }
  WiFi.disconnect(true);
  // A key that is still down would wake the display up again at once
  while (anyKeyDown()) delay(10);
  delay(50);
  uint64_t mask = 0;
  for (int key : KEYS) {
    mask |= 1ULL << key;
    rtc_gpio_pullup_en((gpio_num_t)key);
    rtc_gpio_pulldown_dis((gpio_num_t)key);
  }
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_S * 1000000ULL);
  Serial.printf("Sleeping for %d s\n", SLEEP_INTERVAL_S);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);  // the lowest clock Wi-Fi works with
  for (int key : KEYS) pinMode(key, INPUT_PULLUP);
  epd::begin();
  u8g2.begin(canvas);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(BLACK);
  u8g2.setBackgroundColor(WHITE);
  wakeCause = esp_sleep_get_wakeup_cause();
  bool fromSleep = SLEEP_MODE && (wakeCause == ESP_SLEEP_WAKEUP_TIMER || wakeCause == ESP_SLEEP_WAKEUP_EXT1);
  logEvent(fromSleep ? "Aufgewacht" : "Start (" + resetReason() + ")");

  if (!LittleFS.begin(true)) logEvent("LittleFS fehlt");  // formats the partition on first use

  // Draw the stored data right away, without waiting for Wi-Fi. After deep sleep the panel still
  // shows it, so only the daily refresh draws then.
  load();
  canvas.setRotation(flipped ? 2 : 0);
  if (fromSleep) {
    waitingScreenShown = true;
    if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) secondsSinceRefresh += SLEEP_INTERVAL_S;
    if (secondsSinceRefresh >= 24UL * 60 * 60 && hasContent()) {
      secondsSinceRefresh = 0;
      dailyRefresh();
    }
  } else {
    secondsSinceRefresh = 0;
    if (hasContent()) showCurrent();
  }

  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);  // before mode(), otherwise the default name is used
  WiFi.mode(WIFI_STA);
  // Modem sleep: the radio sleeps between beacons, MQTT stays connected
  WiFi.setSleep(WIFI_PS_MAX_MODEM);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    disconnectReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "fritz.box", "pool.ntp.org");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(IMAGE_SIZE + 256);  // the image plus topic and header
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(onMessage);

  if (SLEEP_MODE && wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    // Which key woke the display up (the lowest GPIO if several)
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    for (int key : KEYS) {
      if (status & (1ULL << key)) {
        handleKey(key);
        break;
      }
    }
  }
  lastActivity = millis();
}

void loop() {
  static bool wasConnected = false, wasMqttConnected = false;
  static uint32_t lastAttempt = 0, lastMqttAttempt = 0, lastRefresh = 0, lastStatus = 0;
  // Once a day: white, then the image again, so the panel does not burn in
  if (!SLEEP_MODE && hasContent() && debugPage < 0 && millis() - lastRefresh > 24UL * 60 * 60 * 1000) {
    lastRefresh = millis();
    dailyRefresh();
  }

  if (disconnectReason >= 0) {
    logEvent("WLAN getrennt, Grund " + String(disconnectReason));
    disconnectReason = -1;
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    logEvent("WLAN verbunden, " + WiFi.localIP().toString());
    if (!hasContent() && !waitingScreenShown && debugPage < 0) waitingScreenShown = renderWaiting();
  }
  if (!connected && wasConnected) wifiDrops++;
  // Auto-reconnect does not retry when the first connection fails, so start over now and then
  if (!connected && millis() - lastAttempt > 30000) {
    lastAttempt = millis();
    Serial.printf("Wi-Fi not connected (status %d), trying again\n", WiFi.status());
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  wasConnected = connected;

  bool mqttConnected = mqtt.connected();
  if (wasMqttConnected && !mqttConnected) {
    mqttDrops++;
    logEvent("MQTT getrennt");
  }
  if (connected && !mqttConnected && (lastMqttAttempt == 0 || millis() - lastMqttAttempt > 10000)) {
    lastMqttAttempt = millis();
    connectMqtt();
  }
  wasMqttConnected = mqtt.connected();
  mqtt.loop();
  if (wasMqttConnected && millis() - lastStatus > 15UL * 60 * 1000) {
    lastStatus = millis();
    publishStatus();
  }

  int key = pressedKey();
  if (key >= 0) {
    handleKey(key);
    lastActivity = millis();
  }
  if (debugPage >= 0 && millis() - debugSince > DEBUG_TIMEOUT) {
    debugPage = -1;
    showCurrent();
    lastActivity = millis();
  }
  if (SLEEP_MODE && debugPage < 0 && (gotMessage || millis() > CYCLE_TIMEOUT) &&
      millis() - lastActivity > IDLE_BEFORE_SLEEP) {
    sleepUntilNextCheck();
  }
  delay(10);
}
