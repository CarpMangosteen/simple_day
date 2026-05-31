#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

namespace {

constexpr uint32_t REFRESH_INTERVAL_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t WIFI_TIMEOUT_MS = 15000UL;
constexpr uint32_t LONG_PRESS_MS = 650UL;
constexpr uint32_t POWER_CHECK_INTERVAL_MS = 60UL * 1000UL;
constexpr uint8_t USB_BRIGHTNESS = 120;
constexpr uint8_t BATTERY_BRIGHTNESS = 70;
constexpr size_t MAX_TODAY = 10;
constexpr size_t MAX_TODOS = 14;
constexpr size_t MAX_DEADLINES = 12;

enum Page { PAGE_NEXT = 0, PAGE_TODAY, PAGE_TODO, PAGE_DEADLINE, PAGE_COUNT };

struct NextItem {
  String title;
  String relative;
  String time;
  String people;
  String where;
};

struct RowItem {
  String left;
  String right;
};

struct FeedState {
  String generatedAt;
  NextItem next;
  RowItem today[MAX_TODAY];
  RowItem todos[MAX_TODOS];
  RowItem deadlines[MAX_DEADLINES];
  size_t todayCount = 0;
  size_t todoCount = 0;
  size_t deadlineCount = 0;
  bool hasNext = false;
  bool online = false;
  bool fromCache = false;
  String message = "Booting";
};

Preferences prefs;
FeedState feed;
Page currentPage = PAGE_NEXT;
int scrollOffset[PAGE_COUNT] = {0, 0, 0, 0};
uint32_t lastRefreshAt = 0;
uint32_t lastPowerCheckAt = 0;
bool sidePressed = false;
bool sideLongHandled = false;
uint32_t sidePressedAt = 0;

String httpUrlWithToken() {
  String url = FEED_URL;
  url += (url.indexOf('?') >= 0) ? "&token=" : "?token=";
  url += DEVICE_TOKEN;
  return url;
}

bool looksConfigured() {
  String ssid = WIFI_SSID;
  String url = FEED_URL;
  return ssid.length() > 0 && ssid.indexOf("YOUR_WIFI") < 0 && url.indexOf("YOUR_SERVER_IP") < 0;
}

void setFont(uint8_t size = 14) {
  if (size <= 12) {
    M5.Display.setFont(&fonts::efontCN_12);
  } else if (size >= 16) {
    M5.Display.setFont(&fonts::efontCN_16);
  } else {
    M5.Display.setFont(&fonts::efontCN_14);
  }
}

int nextUtf8Len(const String &text, int index) {
  if (index >= text.length()) return 0;
  uint8_t c = static_cast<uint8_t>(text[index]);
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

void drawWrapped(const String &text, int x, int y, int width, int lineHeight, uint16_t color, int maxLines) {
  M5.Display.setTextColor(color);
  String line;
  int lines = 0;
  for (int i = 0; i < text.length();) {
    int len = nextUtf8Len(text, i);
    String token = text.substring(i, i + len);
    String candidate = line + token;
    if (line.length() > 0 && M5.Display.textWidth(candidate) > width) {
      M5.Display.drawString(line, x, y + lines * lineHeight);
      lines++;
      line = token;
      if (lines >= maxLines) return;
    } else {
      line = candidate;
    }
    i += len;
  }
  if (line.length() > 0 && lines < maxLines) {
    M5.Display.drawString(line, x, y + lines * lineHeight);
  }
}

String joinPeople(JsonArray people) {
  String joined;
  for (JsonVariant item : people) {
    String value = item.as<String>();
    if (!value.length()) continue;
    if (joined.length()) joined += ",";
    joined += value;
  }
  return joined;
}

bool parseFeedJson(const String &json, bool fromCache) {
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    feed.message = String("JSON error: ") + err.c_str();
    return false;
  }

  FeedState nextFeed;
  nextFeed.generatedAt = doc["generated_at"] | "";
  nextFeed.online = !fromCache;
  nextFeed.fromCache = fromCache;
  nextFeed.message = fromCache ? "Cached" : "Updated";

  JsonObject nextObj = doc["next"].as<JsonObject>();
  if (!nextObj.isNull()) {
    nextFeed.hasNext = true;
    nextFeed.next.title = nextObj["title"] | "";
    nextFeed.next.relative = nextObj["relative"] | "";
    nextFeed.next.time = nextObj["time"] | "";
    nextFeed.next.where = nextObj["where"] | "";
    nextFeed.next.people = joinPeople(nextObj["with"].as<JsonArray>());
  }

  JsonArray today = doc["today"].as<JsonArray>();
  for (JsonObject item : today) {
    if (nextFeed.todayCount >= MAX_TODAY) break;
    nextFeed.today[nextFeed.todayCount++] = {item["time"] | "", item["title"] | ""};
  }

  JsonArray todos = doc["todos"].as<JsonArray>();
  for (JsonObject item : todos) {
    if (nextFeed.todoCount >= MAX_TODOS) break;
    nextFeed.todos[nextFeed.todoCount++] = {"", item["title"] | ""};
  }

  JsonArray deadlines = doc["deadlines"].as<JsonArray>();
  for (JsonObject item : deadlines) {
    if (nextFeed.deadlineCount >= MAX_DEADLINES) break;
    nextFeed.deadlines[nextFeed.deadlineCount++] = {item["title"] | "", item["relative"] | ""};
  }

  feed = nextFeed;
  return true;
}

String sampleJson() {
  return F(
      "{\"generated_at\":\"2026-05-31T09:00:00+08:00\",\"timezone\":\"Asia/Shanghai\","
      "\"next\":{\"title\":\"\\u8bbe\\u8ba1\\u8bc4\\u5ba1\",\"starts_at\":\"2026-05-31T16:00:00+08:00\","
      "\"time\":\"16:00\",\"relative\":\"in 7h\",\"with\":[\"\\u674e\\u5c0f\\u5b87\",\"\\u5c0f\\u59dc\"],"
      "\"where\":\"\\u817e\\u8baf\\u4f1a\\u8bae\"},"
      "\"today\":[{\"time\":\"10:00\",\"title\":\"\\u6668\\u4f1a\"},"
      "{\"time\":\"16:00\",\"title\":\"\\u8bbe\\u8ba1\\u8bc4\\u5ba1\"},"
      "{\"time\":\"19:00\",\"title\":\"\\u665a\\u996d\"}],"
      "\"todos\":[{\"title\":\"\\u6574\\u7406 Simple Day UI \\u8349\\u7a3f\"},"
      "{\"title\":\"\\u786e\\u8ba4\\u670d\\u52a1\\u5668\\u7aef\\u53e3\\u5f00\\u653e\"}],"
      "\"deadlines\":[{\"title\":\"v1.0 simple \\u53d1\\u5e03\",\"relative\":\"in 2d\"},"
      "{\"title\":\"\\u516d\\u6708\\u9884\\u7b97\\u6574\\u7406\",\"relative\":\"in 5d\"},"
      "{\"title\":\"\\u79df\\u623f\\u5408\\u540c\\u5230\\u671f\",\"relative\":\"in 3w\"},"
      "{\"title\":\"\\u7a0e\\u52a1\\u7533\\u62a5\",\"relative\":\"in 6w\"}]}");
}

bool isUsbPowered() {
  int16_t vbus = M5.Power.getVBUSVoltage();
  if (vbus > 4200) return true;
  return M5.Power.isCharging() == m5::Power_Class::is_charging;
}

void applyPowerMode() {
  M5.Display.setBrightness(isUsbPowered() ? USB_BRIGHTNESS : BATTERY_BRIGHTNESS);
  lastPowerCheckAt = millis();
}

bool connectWifi() {
  if (!looksConfigured()) {
    feed.message = "Using sample: edit secrets.h";
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(250);
    M5.update();
  }
  if (WiFi.status() == WL_CONNECTED) {
    feed.online = true;
    return true;
  }
  feed.online = false;
  feed.message = "Wi-Fi failed";
  return false;
}

void syncClock() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org", "time.cloudflare.com");
  struct tm timeinfo;
  getLocalTime(&timeinfo, 2500);
}

bool fetchFeed() {
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    return false;
  }
  syncClock();

  HTTPClient http;
  String url = httpUrlWithToken();
  Serial.printf("GET %s\n", url.c_str());
  if (!http.begin(url)) {
    feed.message = "HTTP begin failed";
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    feed.message = String("HTTP ") + code;
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  if (!parseFeedJson(body, false)) {
    return false;
  }

  prefs.begin("simpleday", false);
  prefs.putString("feed", body);
  prefs.end();
  lastRefreshAt = millis();
  return true;
}

bool loadCachedFeed() {
  prefs.begin("simpleday", true);
  String cached = prefs.getString("feed", "");
  prefs.end();
  if (!cached.length()) return false;
  return parseFeedJson(cached, true);
}

void refreshFeed() {
  feed.message = "Refreshing";
  if (!fetchFeed() && !loadCachedFeed()) {
    parseFeedJson(sampleJson(), true);
  }
  lastRefreshAt = millis();
}

const char *pageTitle(Page page) {
  switch (page) {
    case PAGE_NEXT: return "NEXT";
    case PAGE_TODAY: return "TODAY";
    case PAGE_TODO: return "TODO";
    case PAGE_DEADLINE: return "DEADLINE";
    default: return "";
  }
}

size_t rowCountForPage(Page page) {
  switch (page) {
    case PAGE_TODAY: return feed.todayCount;
    case PAGE_TODO: return feed.todoCount;
    case PAGE_DEADLINE: return feed.deadlineCount;
    default: return 0;
  }
}

void clampScroll(Page page) {
  size_t count = rowCountForPage(page);
  int maxOffset = count > 6 ? static_cast<int>(count - 1) : 0;
  if (scrollOffset[page] < 0) scrollOffset[page] = 0;
  if (scrollOffset[page] > maxOffset) scrollOffset[page] = maxOffset;
}

void drawHeader() {
  M5.Display.fillRect(0, 0, M5.Display.width(), 28, TFT_BLACK);
  M5.Display.drawFastHLine(0, 27, M5.Display.width(), TFT_DARKGREY);
  setFont(16);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.drawString(pageTitle(currentPage), 6, 5);
  setFont(12);
  uint16_t statusColor = feed.online ? TFT_GREEN : (feed.fromCache ? TFT_ORANGE : TFT_RED);
  M5.Display.setTextColor(statusColor);
  M5.Display.drawString(feed.online ? "ON" : (feed.fromCache ? "CACHE" : "OFF"), M5.Display.width() - 46, 7);
}

void drawFooter() {
  M5.Display.drawFastHLine(0, M5.Display.height() - 18, M5.Display.width(), TFT_DARKGREY);
  setFont(12);
  M5.Display.setTextColor(TFT_LIGHTGREY);
  String text = feed.message;
  if (text.length() > 18) text = text.substring(0, 18);
  M5.Display.drawString(text, 6, M5.Display.height() - 15);
  String page = String(static_cast<int>(currentPage) + 1) + "/4";
  M5.Display.drawString(page, M5.Display.width() - 30, M5.Display.height() - 15);
}

void drawNextPage() {
  const int w = M5.Display.width();
  int y = 38;
  if (!feed.hasNext) {
    setFont(16);
    drawWrapped("No upcoming event", 8, y, w - 16, 20, TFT_LIGHTGREY, 4);
    return;
  }

  setFont(16);
  drawWrapped(feed.next.title, 8, y, w - 16, 21, TFT_WHITE, 3);
  y += 66;

  setFont(14);
  M5.Display.setTextColor(TFT_CYAN);
  M5.Display.drawString(feed.next.relative, 8, y);

  int boxW = 58;
  int boxH = 28;
  int boxX = w - boxW - 8;
  M5.Display.drawRoundRect(boxX, y - 5, boxW, boxH, 4, TFT_ORANGE);
  M5.Display.setTextColor(TFT_ORANGE);
  M5.Display.drawString(feed.next.time, boxX + 9, y + 1);
  y += 40;

  setFont(12);
  if (feed.next.people.length()) {
    drawWrapped(String("with ") + feed.next.people, 8, y, w - 16, 16, TFT_LIGHTGREY, 2);
    y += 34;
  }
  if (feed.next.where.length()) {
    drawWrapped(feed.next.where, 8, y, w - 16, 16, TFT_LIGHTGREY, 2);
  }
}

void drawRows(RowItem *items, size_t count, bool arrows, bool rightAligned) {
  if (count == 0) {
    setFont(14);
    drawWrapped("Empty", 8, 44, M5.Display.width() - 16, 18, TFT_LIGHTGREY, 2);
    return;
  }

  clampScroll(currentPage);
  int y = 36;
  int start = scrollOffset[currentPage];
  int rowH = 28;
  int bottom = M5.Display.height() - 22;
  setFont(12);
  for (size_t i = start; i < count && y + rowH <= bottom; ++i) {
    M5.Display.drawFastHLine(0, y + rowH - 2, M5.Display.width(), TFT_DARKGREY);
    M5.Display.setTextColor(TFT_WHITE);
    int textX = 8;
    if (arrows) {
      M5.Display.setTextColor(TFT_CYAN);
      M5.Display.drawString(">", 8, y + 5);
      textX = 22;
    }

    if (rightAligned) {
      M5.Display.setTextColor(TFT_WHITE);
      drawWrapped(items[i].left, textX, y + 4, M5.Display.width() - textX - 44, 14, TFT_WHITE, 1);
      M5.Display.setTextColor(TFT_ORANGE);
      M5.Display.drawString(items[i].right, M5.Display.width() - 38, y + 4);
    } else {
      String line = items[i].left.length() ? items[i].left + " " + items[i].right : items[i].right;
      drawWrapped(line, textX, y + 4, M5.Display.width() - textX - 6, 14, TFT_WHITE, 1);
    }
    y += rowH;
  }
}

void drawCurrentPage() {
  M5.Display.fillScreen(TFT_BLACK);
  drawHeader();
  switch (currentPage) {
    case PAGE_NEXT:
      drawNextPage();
      break;
    case PAGE_TODAY:
      drawRows(feed.today, feed.todayCount, false, false);
      break;
    case PAGE_TODO:
      drawRows(feed.todos, feed.todoCount, true, false);
      break;
    case PAGE_DEADLINE:
      drawRows(feed.deadlines, feed.deadlineCount, false, true);
      break;
    default:
      break;
  }
  drawFooter();
}

void nextPage() {
  currentPage = static_cast<Page>((static_cast<int>(currentPage) + 1) % PAGE_COUNT);
  scrollOffset[currentPage] = 0;
  drawCurrentPage();
}

void scrollDown() {
  if (currentPage == PAGE_NEXT) return;
  scrollOffset[currentPage]++;
  clampScroll(currentPage);
  drawCurrentPage();
}

void scrollUp() {
  if (currentPage == PAGE_NEXT) return;
  scrollOffset[currentPage]--;
  clampScroll(currentPage);
  drawCurrentPage();
}

void handleButtons() {
  M5.update();
  if (M5.BtnA.wasPressed()) {
    nextPage();
  }

  if (M5.BtnB.wasPressed()) {
    sidePressed = true;
    sideLongHandled = false;
    sidePressedAt = millis();
  }

  if (sidePressed && M5.BtnB.isPressed() && !sideLongHandled && millis() - sidePressedAt >= LONG_PRESS_MS) {
    sideLongHandled = true;
    scrollUp();
  }

  if (sidePressed && M5.BtnB.wasReleased()) {
    if (!sideLongHandled) scrollDown();
    sidePressed = false;
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.fallback_board = m5::board_t::board_M5StickS3;
  M5.begin(cfg);
  Serial.begin(115200);
  delay(200);

  M5.Display.setRotation(0);
  applyPowerMode();
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextWrap(false);

  M5.Display.fillScreen(TFT_BLACK);
  setFont(14);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.drawString("Simple Day", 8, 8);
  M5.Display.drawString("Starting...", 8, 30);

  Serial.println("Simple Day StickS3 boot");
  refreshFeed();
  drawCurrentPage();
}

void loop() {
  handleButtons();
  if (millis() - lastPowerCheckAt >= POWER_CHECK_INTERVAL_MS) {
    applyPowerMode();
  }
  if (millis() - lastRefreshAt >= REFRESH_INTERVAL_MS) {
    refreshFeed();
    drawCurrentPage();
  }
  delay(20);
}
