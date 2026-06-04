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
constexpr size_t MAX_SHOPPING = 12;
constexpr size_t MAX_DEADLINES = 12;
constexpr int LIST_TOP_Y = 36;
constexpr int LIST_BOTTOM_MARGIN = 22;
constexpr int TODO_TEXT_X = 22;
constexpr int TODO_MAX_LINES = 3;
constexpr int TODO_LINE_HEIGHT = 16;
constexpr int TODO_ROW_PADDING = 10;

enum Page { PAGE_NEXT = 0, PAGE_TODAY, PAGE_TODO, PAGE_SHOPPING, PAGE_DEADLINE, PAGE_COUNT };
enum WindowMode { MODE_LIFE = 0, MODE_PROJECT, MODE_COUNT };

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

struct TodoItem {
  int id = 0;
  bool completed = false;
  String title;
};

struct ShoppingItem {
  int id = 0;
  bool completed = false;
  String title;
};

struct FeedBucket {
  NextItem next;
  RowItem today[MAX_TODAY];
  TodoItem todos[MAX_TODOS];
  ShoppingItem shopping[MAX_SHOPPING];
  size_t todayCount = 0;
  size_t todoCount = 0;
  size_t shoppingCount = 0;
  bool hasNext = false;
};

struct FeedState {
  String generatedAt;
  FeedBucket life;
  FeedBucket project;
  RowItem deadlines[MAX_DEADLINES];
  size_t deadlineCount = 0;
  bool online = false;
  bool fromCache = false;
  String message = "Booting";
};

Preferences prefs;
FeedState feed;
WindowMode currentMode = MODE_LIFE;
Page currentPage = PAGE_NEXT;
int scrollOffset[MODE_COUNT][PAGE_COUNT] = {};
uint32_t lastRefreshAt = 0;
uint32_t lastPowerCheckAt = 0;
bool sidePressed = false;
bool sideLongHandled = false;
uint32_t sidePressedAt = 0;
bool aPressed = false;
bool aLongHandled = false;
uint32_t aPressedAt = 0;
int todoSelected[MODE_COUNT] = {0, 0};
int shoppingSelected[MODE_COUNT] = {0, 0};
int lastBatteryPercent = -1;
bool lastUsbPowered = false;

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

int countWrappedLines(const String &text, int width, int maxLines) {
  if (maxLines <= 0) return 0;
  if (text.isEmpty()) return 1;
  String line;
  int lines = 0;
  for (int i = 0; i < text.length();) {
    int len = nextUtf8Len(text, i);
    String token = text.substring(i, i + len);
    String candidate = line + token;
    if (line.length() > 0 && M5.Display.textWidth(candidate) > width) {
      lines++;
      line = token;
      if (lines >= maxLines) return lines;
    } else {
      line = candidate;
    }
    i += len;
  }
  if (line.length() > 0 && lines < maxLines) {
    lines++;
  }
  return lines > 0 ? lines : 1;
}

String fitTextToWidth(const String &text, int maxWidth) {
  if (maxWidth <= 0) return "";
  if (M5.Display.textWidth(text) <= maxWidth) return text;
  String trimmed = text;
  while (trimmed.length() > 0 && M5.Display.textWidth(trimmed + "...") > maxWidth) {
    trimmed.remove(trimmed.length() - 1);
  }
  return trimmed.length() ? trimmed + "..." : "";
}

const Page LIFE_PAGES[] = {PAGE_NEXT, PAGE_TODAY, PAGE_TODO, PAGE_SHOPPING, PAGE_DEADLINE};
const Page PROJECT_PAGES[] = {PAGE_NEXT, PAGE_TODAY, PAGE_TODO, PAGE_DEADLINE};

int modeIndex(WindowMode mode) {
  return static_cast<int>(mode);
}

int pageCountForMode(WindowMode mode) {
  return mode == MODE_LIFE ? static_cast<int>(sizeof(LIFE_PAGES) / sizeof(Page))
                            : static_cast<int>(sizeof(PROJECT_PAGES) / sizeof(Page));
}

Page pageAtIndex(WindowMode mode, int index) {
  if (mode == MODE_LIFE) {
    return LIFE_PAGES[index % pageCountForMode(mode)];
  }
  return PROJECT_PAGES[index % pageCountForMode(mode)];
}

int pageIndexForMode(WindowMode mode, Page page) {
  int count = pageCountForMode(mode);
  for (int i = 0; i < count; ++i) {
    if (pageAtIndex(mode, i) == page) return i;
  }
  return 0;
}

FeedBucket &bucketForMode(WindowMode mode) {
  return mode == MODE_LIFE ? feed.life : feed.project;
}

FeedBucket &currentBucket() {
  return bucketForMode(currentMode);
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

void parseBucket(JsonObject obj, FeedBucket &bucket) {
  if (obj.isNull()) return;

  JsonObject nextObj = obj["next"].as<JsonObject>();
  if (!nextObj.isNull()) {
    bucket.hasNext = true;
    bucket.next.title = nextObj["title"] | "";
    bucket.next.relative = nextObj["relative"] | "";
    bucket.next.time = nextObj["time"] | "";
    bucket.next.where = nextObj["where"] | "";
    bucket.next.people = joinPeople(nextObj["with"].as<JsonArray>());
  }

  JsonArray today = obj["today"].as<JsonArray>();
  for (JsonObject item : today) {
    if (bucket.todayCount >= MAX_TODAY) break;
    bucket.today[bucket.todayCount++] = {item["time"] | "", item["title"] | ""};
  }

  JsonArray todos = obj["todos"].as<JsonArray>();
  for (JsonObject item : todos) {
    if (bucket.todoCount >= MAX_TODOS) break;
    TodoItem todo;
    todo.id = item["id"] | 0;
    todo.title = item["title"] | "";
    todo.completed = item["completed"] | false;
    bucket.todos[bucket.todoCount++] = todo;
  }

  JsonArray shopping = obj["shopping"].as<JsonArray>();
  for (JsonObject item : shopping) {
    if (bucket.shoppingCount >= MAX_SHOPPING) break;
    ShoppingItem shop;
    shop.id = item["id"] | 0;
    shop.title = item["title"] | "";
    shop.completed = item["completed"] | false;
    bucket.shopping[bucket.shoppingCount++] = shop;
  }
}

bool parseFeedJson(const String &json, bool fromCache) {
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    feed.message = String("JSON error: ") + err.c_str();
    return false;
  }

  FeedState nextFeed{};
  nextFeed.generatedAt = doc["generated_at"] | "";
  nextFeed.online = !fromCache;
  nextFeed.fromCache = fromCache;
  nextFeed.message = fromCache ? "Cached" : "Updated";

  JsonObject lifeObj = doc["life"].as<JsonObject>();
  parseBucket(lifeObj, nextFeed.life);

  JsonObject projectObj = doc["project"].as<JsonObject>();
  parseBucket(projectObj, nextFeed.project);

  JsonArray deadlines = doc["deadlines"].as<JsonArray>();
  for (JsonObject item : deadlines) {
    if (nextFeed.deadlineCount >= MAX_DEADLINES) break;
    nextFeed.deadlines[nextFeed.deadlineCount++] = {item["title"] | "", item["relative"] | ""};
  }

  feed = nextFeed;
  for (int mode = 0; mode < MODE_COUNT; ++mode) {
    FeedBucket &bucket = bucketForMode(static_cast<WindowMode>(mode));
    if (bucket.todoCount == 0) {
      todoSelected[mode] = 0;
      scrollOffset[mode][PAGE_TODO] = 0;
    } else if (todoSelected[mode] >= static_cast<int>(bucket.todoCount)) {
      todoSelected[mode] = static_cast<int>(bucket.todoCount) - 1;
    }

    if (bucket.shoppingCount == 0) {
      shoppingSelected[mode] = 0;
      scrollOffset[mode][PAGE_SHOPPING] = 0;
    } else if (shoppingSelected[mode] >= static_cast<int>(bucket.shoppingCount)) {
      shoppingSelected[mode] = static_cast<int>(bucket.shoppingCount) - 1;
    }
  }

  if (currentMode == MODE_PROJECT && currentPage == PAGE_SHOPPING) {
    currentPage = PAGE_NEXT;
  }
  return true;
}

String sampleJson() {
  return F(
      "{\"generated_at\":\"2026-05-31T09:00:00+08:00\",\"timezone\":\"Asia/Shanghai\","
  "\"life\":{"
  "\"next\":{\"title\":\"\\u6668\\u4f1a\",\"starts_at\":\"2026-05-31T10:00:00+08:00\","
  "\"time\":\"10:00\",\"relative\":\"in 1h\",\"with\":[],\"where\":\"\"},"
  "\"today\":[{\"time\":\"10:00\",\"title\":\"\\u6668\\u4f1a\"}],"
  "\"todos\":[{\"id\":1,\"title\":\"\\u6574\\u7406 \\u751f\\u6d3b TODO\",\"completed\":false}],"
  "\"shopping\":[{\"id\":1,\"title\":\"\\u725b\\u5976\",\"completed\":false}]},"
  "\"project\":{"
  "\"next\":{\"title\":\"\\u8bbe\\u8ba1\\u8bc4\\u5ba1\",\"starts_at\":\"2026-05-31T16:00:00+08:00\","
  "\"time\":\"16:00\",\"relative\":\"in 7h\",\"with\":[\"\\u674e\\u5c0f\\u5b87\",\"\\u5c0f\\u59dc\"],"
  "\"where\":\"\\u817e\\u8baf\\u4f1a\\u8bae\"},"
  "\"today\":[{\"time\":\"16:00\",\"title\":\"\\u8bbe\\u8ba1\\u8bc4\\u5ba1\"}],"
  "\"todos\":[{\"id\":2,\"title\":\"\\u786e\\u8ba4 \\u670d\\u52a1\\u5668\\u7aef\\u53e3\",\"completed\":false}]},"
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

int readBatteryPercent() {
  int level = M5.Power.getBatteryLevel();
  if (level < 0) return -1;
  if (level > 100) level = 100;
  return level;
}

void updateBatteryStatus(bool usbPowered) {
  lastUsbPowered = usbPowered;
  if (usbPowered) {
    lastBatteryPercent = 100;
    return;
  }
  int level = readBatteryPercent();
  if (level >= 0) {
    lastBatteryPercent = level;
  }
}

void drawLightningIcon(int x, int y, uint16_t color) {
  M5.Display.drawLine(x + 4, y, x + 1, y + 5, color);
  M5.Display.drawLine(x + 1, y + 5, x + 4, y + 5, color);
  M5.Display.drawLine(x + 4, y + 5, x + 2, y + 10, color);
}

void drawModeIndicator() {
  uint16_t color = currentMode == MODE_PROJECT ? TFT_NAVY : TFT_DARKGREEN;
  M5.Display.fillCircle(3, 14, 2, color);
}

void applyPowerMode() {
  bool usbPowered = isUsbPowered();
  M5.Display.setBrightness(usbPowered ? USB_BRIGHTNESS : BATTERY_BRIGHTNESS);
  updateBatteryStatus(usbPowered);
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
    case PAGE_SHOPPING: return "SHOPPING";
    case PAGE_DEADLINE: return "DEADLINE";
    default: return "";
  }
}

size_t rowCountForPage(Page page) {
  FeedBucket &bucket = currentBucket();
  switch (page) {
    case PAGE_TODAY: return bucket.todayCount;
    case PAGE_TODO: return bucket.todoCount;
    case PAGE_SHOPPING: return currentMode == MODE_LIFE ? bucket.shoppingCount : 0;
    case PAGE_DEADLINE: return feed.deadlineCount;
    default: return 0;
  }
}

int visibleRowsForPage(Page page) {
  int top = LIST_TOP_Y;
  int bottom = M5.Display.height() - LIST_BOTTOM_MARGIN;
  int available = bottom - top;
  int rowH = 28;
  if (page == PAGE_TODO || page == PAGE_SHOPPING) {
    rowH = TODO_MAX_LINES * TODO_LINE_HEIGHT + TODO_ROW_PADDING;
  }
  if (rowH <= 0) return 1;
  int visible = available / rowH;
  return visible > 0 ? visible : 1;
}

void clampScroll(Page page) {
  int mode = modeIndex(currentMode);
  size_t count = rowCountForPage(page);
  int visible = visibleRowsForPage(page);
  int maxOffset = count > static_cast<size_t>(visible) ? static_cast<int>(count - visible) : 0;
  if (scrollOffset[mode][page] < 0) scrollOffset[mode][page] = 0;
  if (scrollOffset[mode][page] > maxOffset) scrollOffset[mode][page] = maxOffset;
}

void drawHeader() {
  M5.Display.fillRect(0, 0, M5.Display.width(), 28, TFT_BLACK);
  M5.Display.drawFastHLine(0, 27, M5.Display.width(), TFT_DARKGREY);
  drawModeIndicator();
  setFont(12);
  uint16_t statusColor = feed.online ? TFT_GREEN : (feed.fromCache ? TFT_ORANGE : TFT_RED);
  String statusText = feed.online ? "ON" : (feed.fromCache ? "CACHE" : "OFF");
  int statusWidth = M5.Display.textWidth(statusText);
  int statusX = M5.Display.width() - statusWidth - 6;
  int batteryPercent = lastBatteryPercent;
  if (batteryPercent < 0 && lastUsbPowered) batteryPercent = 100;
  bool showStatus = true;
  bool showBattery = batteryPercent >= 0;
  int boltW = (lastUsbPowered && showBattery) ? 6 : 0;
  int boltGap = (lastUsbPowered && showBattery) ? 4 : 0;
  String pctText = String(batteryPercent) + "%";
  int pctWidth = showBattery ? M5.Display.textWidth(pctText) : 0;

  auto calcRightLimit = [&](int &statusXOut, int &batteryXOut) {
    int rightPadding = 6;
    int rightLimit = M5.Display.width() - rightPadding;
    statusXOut = -1;
    batteryXOut = -1;
    if (showStatus) {
      statusXOut = M5.Display.width() - statusWidth - rightPadding;
      rightLimit = statusXOut - 6;
    }
    if (showBattery) {
      int batteryWidth = pctWidth + boltW + boltGap;
      int batteryRight = showStatus ? (statusXOut - 8) : (M5.Display.width() - rightPadding);
      batteryXOut = batteryRight - batteryWidth;
      if (batteryXOut < 6) {
        batteryXOut = -1;
      } else {
        rightLimit = min(rightLimit, batteryXOut - 6);
      }
    }
    return rightLimit;
  };

  int batteryX = -1;
  int rightLimit = calcRightLimit(statusX, batteryX);

  setFont(16);
  int titleWidth = M5.Display.textWidth(pageTitle(currentPage));
  if (titleWidth > rightLimit - 6 && showBattery) {
    showBattery = false;
    boltW = 0;
    boltGap = 0;
    pctWidth = 0;
    rightLimit = calcRightLimit(statusX, batteryX);
  }
  if (titleWidth > rightLimit - 6 && showStatus) {
    showStatus = false;
    rightLimit = calcRightLimit(statusX, batteryX);
  }

  uint8_t titleFont = 16;
  if (titleWidth > rightLimit - 6) {
    titleFont = 14;
    setFont(titleFont);
    titleWidth = M5.Display.textWidth(pageTitle(currentPage));
  }
  if (titleWidth > rightLimit - 6) {
    titleFont = 12;
    setFont(titleFont);
  }

  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.drawString(pageTitle(currentPage), 6, (titleFont == 12 ? 8 : 6));

  setFont(12);
  if (showStatus && statusX >= 0) {
    M5.Display.setTextColor(statusColor);
    M5.Display.drawString(statusText, statusX, 7);
  }

  if (showBattery && batteryX >= 0) {
    uint16_t iconColor = lastUsbPowered ? TFT_GREEN : TFT_LIGHTGREY;
    M5.Display.setTextColor(iconColor);
    int textX = batteryX;
    if (boltW > 0) {
      drawLightningIcon(textX, 7, iconColor);
      textX += boltW + boltGap;
    }
    M5.Display.drawString(pctText, textX, 7);
  }
}

void drawFooter() {
  M5.Display.drawFastHLine(0, M5.Display.height() - 18, M5.Display.width(), TFT_DARKGREY);
  setFont(12);
  M5.Display.setTextColor(TFT_LIGHTGREY);
  String text = feed.message;
  if (text.length() > 18) text = text.substring(0, 18);
  M5.Display.drawString(text, 6, M5.Display.height() - 15);
  int index = pageIndexForMode(currentMode, currentPage);
  String page = String(index + 1) + "/" + String(pageCountForMode(currentMode));
  M5.Display.drawString(page, M5.Display.width() - 30, M5.Display.height() - 15);
}

void drawNextPage() {
  const int w = M5.Display.width();
  int y = 38;
  FeedBucket &bucket = currentBucket();
  if (!bucket.hasNext) {
    setFont(16);
    drawWrapped("No upcoming event", 8, y, w - 16, 20, TFT_LIGHTGREY, 4);
    return;
  }

  setFont(16);
  drawWrapped(bucket.next.title, 8, y, w - 16, 21, TFT_WHITE, 3);
  y += 66;

  setFont(14);
  M5.Display.setTextColor(TFT_CYAN);
  M5.Display.drawString(bucket.next.relative, 8, y);
  y += 22;

  setFont(16);
  int boxH = 30;
  int boxW = max(72, M5.Display.textWidth(bucket.next.time) + 26);
  int boxX = (w - boxW) / 2;
  int boxY = y + 2;
  M5.Display.drawRoundRect(boxX, boxY, boxW, boxH, 5, TFT_ORANGE);
  M5.Display.setTextColor(TFT_ORANGE);
  int textW = M5.Display.textWidth(bucket.next.time);
  int textX = boxX + (boxW - textW) / 2;
  M5.Display.drawString(bucket.next.time, textX, boxY + 7);
  y = boxY + boxH + 12;

  setFont(12);
  if (bucket.next.people.length()) {
    drawWrapped(String("with ") + bucket.next.people, 8, y, w - 16, 16, TFT_LIGHTGREY, 2);
    y += 34;
  }
  if (bucket.next.where.length()) {
    drawWrapped(bucket.next.where, 8, y, w - 16, 16, TFT_LIGHTGREY, 2);
  }
}

void drawRows(RowItem *items, size_t count, bool arrows, bool rightAligned) {
  if (count == 0) {
    setFont(14);
    drawWrapped("Empty", 8, 44, M5.Display.width() - 16, 18, TFT_LIGHTGREY, 2);
    return;
  }

  clampScroll(currentPage);
  int y = LIST_TOP_Y;
  int start = scrollOffset[modeIndex(currentMode)][currentPage];
  int rowH = 28;
  int bottom = M5.Display.height() - LIST_BOTTOM_MARGIN;
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

int todoTextWidth() {
  return M5.Display.width() - TODO_TEXT_X - 6;
}

int todoRowHeight(const String &text) {
  int lines = countWrappedLines(text, todoTextWidth(), TODO_MAX_LINES);
  return lines * TODO_LINE_HEIGHT + TODO_ROW_PADDING;
}

int lastVisibleTodoIndex(int start) {
  FeedBucket &bucket = currentBucket();
  if (bucket.todoCount == 0) return -1;
  setFont(12);
  int y = LIST_TOP_Y;
  int bottom = M5.Display.height() - LIST_BOTTOM_MARGIN;
  int last = start - 1;
  for (int i = start; i < static_cast<int>(bucket.todoCount); ++i) {
    int rowH = todoRowHeight(bucket.todos[i].title);
    if (y + rowH > bottom) break;
    y += rowH;
    last = i;
  }
  return last < start ? start : last;
}

void ensureTodoVisible() {
  int mode = modeIndex(currentMode);
  FeedBucket &bucket = currentBucket();
  if (bucket.todoCount == 0) {
    scrollOffset[mode][PAGE_TODO] = 0;
    todoSelected[mode] = 0;
    return;
  }
  if (todoSelected[mode] < 0) todoSelected[mode] = 0;
  if (todoSelected[mode] >= static_cast<int>(bucket.todoCount)) {
    todoSelected[mode] = static_cast<int>(bucket.todoCount) - 1;
  }
  if (todoSelected[mode] < scrollOffset[mode][PAGE_TODO]) {
    scrollOffset[mode][PAGE_TODO] = todoSelected[mode];
    return;
  }
  int last = lastVisibleTodoIndex(scrollOffset[mode][PAGE_TODO]);
  if (todoSelected[mode] > last) {
    scrollOffset[mode][PAGE_TODO] = todoSelected[mode];
  }
}

void drawStrikeThrough(int x, int y, int width, int lines, int lineHeight, uint16_t color) {
  for (int i = 0; i < lines; ++i) {
    int yLine = y + i * lineHeight + lineHeight / 2;
    M5.Display.drawFastHLine(x, yLine, width, color);
  }
}

void drawTodoPage() {
  FeedBucket &bucket = currentBucket();
  int mode = modeIndex(currentMode);
  if (bucket.todoCount == 0) {
    setFont(14);
    drawWrapped("Empty", 8, 44, M5.Display.width() - 16, 18, TFT_LIGHTGREY, 2);
    return;
  }

  clampScroll(PAGE_TODO);
  ensureTodoVisible();
  int y = LIST_TOP_Y;
  int start = scrollOffset[mode][PAGE_TODO];
  int bottom = M5.Display.height() - LIST_BOTTOM_MARGIN;
  setFont(12);
  for (int i = start; i < static_cast<int>(bucket.todoCount); ++i) {
    int lines = countWrappedLines(bucket.todos[i].title, todoTextWidth(), TODO_MAX_LINES);
    int rowH = lines * TODO_LINE_HEIGHT + TODO_ROW_PADDING;
    if (y + rowH > bottom) break;
    bool selected = (i == todoSelected[mode]);
    uint16_t textColor = bucket.todos[i].completed ? TFT_LIGHTGREY : TFT_WHITE;
    if (selected) {
      M5.Display.fillRect(0, y, M5.Display.width(), rowH, TFT_DARKGREY);
      M5.Display.fillRect(0, y, 4, rowH, TFT_CYAN);
    }
    M5.Display.drawFastHLine(0, y + rowH - 2, M5.Display.width(), TFT_DARKGREY);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.drawString(">", 8, y + 5);
    M5.Display.setTextColor(textColor);
    int textY = y + 4;
    drawWrapped(bucket.todos[i].title, TODO_TEXT_X, textY, todoTextWidth(), TODO_LINE_HEIGHT, textColor,
                TODO_MAX_LINES);
    if (bucket.todos[i].completed) {
      drawStrikeThrough(TODO_TEXT_X, textY, todoTextWidth(), lines, TODO_LINE_HEIGHT, TFT_LIGHTGREY);
    }
    y += rowH;
  }
}

int lastVisibleShoppingIndex(int start) {
  FeedBucket &bucket = currentBucket();
  if (bucket.shoppingCount == 0) return -1;
  setFont(12);
  int y = LIST_TOP_Y;
  int bottom = M5.Display.height() - LIST_BOTTOM_MARGIN;
  int last = start - 1;
  for (int i = start; i < static_cast<int>(bucket.shoppingCount); ++i) {
    int rowH = todoRowHeight(bucket.shopping[i].title);
    if (y + rowH > bottom) break;
    y += rowH;
    last = i;
  }
  return last < start ? start : last;
}

void ensureShoppingVisible() {
  int mode = modeIndex(currentMode);
  FeedBucket &bucket = currentBucket();
  if (bucket.shoppingCount == 0) {
    scrollOffset[mode][PAGE_SHOPPING] = 0;
    shoppingSelected[mode] = 0;
    return;
  }
  if (shoppingSelected[mode] < 0) shoppingSelected[mode] = 0;
  if (shoppingSelected[mode] >= static_cast<int>(bucket.shoppingCount)) {
    shoppingSelected[mode] = static_cast<int>(bucket.shoppingCount) - 1;
  }
  if (shoppingSelected[mode] < scrollOffset[mode][PAGE_SHOPPING]) {
    scrollOffset[mode][PAGE_SHOPPING] = shoppingSelected[mode];
    return;
  }
  int last = lastVisibleShoppingIndex(scrollOffset[mode][PAGE_SHOPPING]);
  if (shoppingSelected[mode] > last) {
    scrollOffset[mode][PAGE_SHOPPING] = shoppingSelected[mode];
  }
}

void drawShoppingPage() {
  FeedBucket &bucket = currentBucket();
  int mode = modeIndex(currentMode);
  if (bucket.shoppingCount == 0) {
    setFont(14);
    drawWrapped("Empty", 8, 44, M5.Display.width() - 16, 18, TFT_LIGHTGREY, 2);
    return;
  }

  clampScroll(PAGE_SHOPPING);
  ensureShoppingVisible();
  int y = LIST_TOP_Y;
  int start = scrollOffset[mode][PAGE_SHOPPING];
  int bottom = M5.Display.height() - LIST_BOTTOM_MARGIN;
  setFont(12);
  for (int i = start; i < static_cast<int>(bucket.shoppingCount); ++i) {
    int lines = countWrappedLines(bucket.shopping[i].title, todoTextWidth(), TODO_MAX_LINES);
    int rowH = lines * TODO_LINE_HEIGHT + TODO_ROW_PADDING;
    if (y + rowH > bottom) break;
    bool selected = (i == shoppingSelected[mode]);
    uint16_t textColor = bucket.shopping[i].completed ? TFT_LIGHTGREY : TFT_WHITE;
    if (selected) {
      M5.Display.fillRect(0, y, M5.Display.width(), rowH, TFT_DARKGREY);
      M5.Display.fillRect(0, y, 4, rowH, TFT_CYAN);
    }
    M5.Display.drawFastHLine(0, y + rowH - 2, M5.Display.width(), TFT_DARKGREY);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.drawString(">", 8, y + 5);
    M5.Display.setTextColor(textColor);
    int textY = y + 4;
    drawWrapped(bucket.shopping[i].title, TODO_TEXT_X, textY, todoTextWidth(), TODO_LINE_HEIGHT, textColor,
                TODO_MAX_LINES);
    if (bucket.shopping[i].completed) {
      drawStrikeThrough(TODO_TEXT_X, textY, todoTextWidth(), lines, TODO_LINE_HEIGHT, TFT_LIGHTGREY);
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
      drawRows(currentBucket().today, currentBucket().todayCount, false, false);
      break;
    case PAGE_TODO:
      drawTodoPage();
      break;
    case PAGE_SHOPPING:
      drawShoppingPage();
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
  int index = pageIndexForMode(currentMode, currentPage);
  currentPage = pageAtIndex(currentMode, index + 1);
  int mode = modeIndex(currentMode);
  scrollOffset[mode][currentPage] = 0;
  if (currentPage == PAGE_TODO) {
    todoSelected[mode] = 0;
  } else if (currentPage == PAGE_SHOPPING) {
    shoppingSelected[mode] = 0;
  }
  drawCurrentPage();
}

void toggleMode() {
  currentMode = currentMode == MODE_LIFE ? MODE_PROJECT : MODE_LIFE;
  if (currentMode == MODE_PROJECT && currentPage == PAGE_SHOPPING) {
    currentPage = PAGE_NEXT;
  }
  drawCurrentPage();
}

void scrollDown() {
  if (currentPage == PAGE_NEXT) return;
  scrollOffset[modeIndex(currentMode)][currentPage]++;
  clampScroll(currentPage);
  drawCurrentPage();
}

void scrollUp() {
  if (currentPage == PAGE_NEXT) return;
  scrollOffset[modeIndex(currentMode)][currentPage]--;
  clampScroll(currentPage);
  drawCurrentPage();
}

String deviceBaseUrl() {
  String url = FEED_URL;
  int index = url.indexOf("/api/device/");
  if (index < 0) return url;
  return url.substring(0, index) + "/api/device";
}

String deviceUrlWithToken(const String &path) {
  String url = deviceBaseUrl() + path;
  url += (url.indexOf('?') >= 0) ? "&token=" : "?token=";
  url += DEVICE_TOKEN;
  return url;
}

bool patchTodoCompleted(int id, bool completed) {
  if (id <= 0) {
    feed.message = completed ? "Marked done" : "Reopened";
    return true;
  }
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    return false;
  }
  HTTPClient http;
  String url = deviceUrlWithToken(String("/todos/") + String(id));
  if (!http.begin(url)) {
    feed.message = "HTTP begin failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  String payload = String("{\"completed\":") + (completed ? "true" : "false") + "}";
  int code = http.sendRequest("PATCH", payload);
  http.end();
  if (code != HTTP_CODE_OK) {
    feed.message = String("HTTP ") + code;
    return false;
  }
  feed.message = completed ? "Marked done" : "Reopened";
  return true;
}

bool patchShoppingCompleted(int id, bool completed) {
  if (id <= 0) {
    feed.message = completed ? "Marked done" : "Reopened";
    return true;
  }
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    return false;
  }
  HTTPClient http;
  String url = deviceUrlWithToken(String("/shopping/") + String(id));
  if (!http.begin(url)) {
    feed.message = "HTTP begin failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  String payload = String("{\"completed\":") + (completed ? "true" : "false") + "}";
  int code = http.sendRequest("PATCH", payload);
  http.end();
  if (code != HTTP_CODE_OK) {
    feed.message = String("HTTP ") + code;
    return false;
  }
  feed.message = completed ? "Marked done" : "Reopened";
  return true;
}

void handleButtons() {
  M5.update();
  if (M5.BtnA.wasPressed()) {
    aPressed = true;
    aLongHandled = false;
    aPressedAt = millis();
  }

  if (aPressed && M5.BtnA.isPressed() && !aLongHandled && millis() - aPressedAt >= LONG_PRESS_MS) {
    aLongHandled = true;
    toggleMode();
  }

  if (aPressed && M5.BtnA.wasReleased()) {
    if (!aLongHandled) {
      nextPage();
    }
    aPressed = false;
  }

  if (M5.BtnB.wasPressed()) {
    sidePressed = true;
    sideLongHandled = false;
    sidePressedAt = millis();
  }

  if (sidePressed && M5.BtnB.isPressed() && !sideLongHandled && millis() - sidePressedAt >= LONG_PRESS_MS) {
    sideLongHandled = true;
    if (currentPage == PAGE_TODO) {
      FeedBucket &bucket = currentBucket();
      int mode = modeIndex(currentMode);
      if (bucket.todoCount > 0) {
        TodoItem &item = bucket.todos[todoSelected[mode]];
        bool nextState = !item.completed;
        item.completed = nextState;
        drawCurrentPage();
        if (!patchTodoCompleted(item.id, nextState)) {
          feed.message = "Sync failed";
        }
        drawFooter();
      }
    } else if (currentPage == PAGE_SHOPPING) {
      FeedBucket &bucket = currentBucket();
      int mode = modeIndex(currentMode);
      if (bucket.shoppingCount > 0) {
        ShoppingItem &item = bucket.shopping[shoppingSelected[mode]];
        bool nextState = !item.completed;
        item.completed = nextState;
        drawCurrentPage();
        if (!patchShoppingCompleted(item.id, nextState)) {
          feed.message = "Sync failed";
        }
        drawFooter();
      }
    } else {
      scrollUp();
    }
  }

  if (sidePressed && M5.BtnB.wasReleased()) {
    if (!sideLongHandled) {
      if (currentPage == PAGE_TODO) {
        FeedBucket &bucket = currentBucket();
        int mode = modeIndex(currentMode);
        if (bucket.todoCount > 0) {
          todoSelected[mode] = (todoSelected[mode] + 1) % static_cast<int>(bucket.todoCount);
          ensureTodoVisible();
          drawCurrentPage();
        }
      } else if (currentPage == PAGE_SHOPPING) {
        FeedBucket &bucket = currentBucket();
        int mode = modeIndex(currentMode);
        if (bucket.shoppingCount > 0) {
          shoppingSelected[mode] = (shoppingSelected[mode] + 1) % static_cast<int>(bucket.shoppingCount);
          ensureShoppingVisible();
          drawCurrentPage();
        }
      } else {
        scrollDown();
      }
    }
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
