/* ============================================================================
   Claude 套餐用量仪表盘 (双页) —— Waveshare ESP32-S3-Touch-LCD-1.85B (ST77916 360x360)
   Page 1 概览: SESSION / WEEKLY 大表 + 重置倒计时。
   Page 2 细节: 各档限额(5H/WEEK/SONNET/OPUS/EXTRA) + 今日 token + 当前活跃会话。
   按 BOOT 键(GPIO0)翻页。数据来自 claude_limits_proxy.py。

   FQBN: esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=16M,PSRAM=opi
   (PSRAM=opi 给 Arduino_Canvas 帧缓冲；右上角电量来自 BQ27220 @0x55)
   ============================================================================ */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "st77916_ws_init.h"
#include "claude_logo.h"

// ---------------------- 1) EDIT THESE / 改这里 ----------------------
const char* WIFI_SSID = "YOUR_WIFI_SSID";       // 2.4GHz only (ESP32-S3 can't do 5GHz)
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* PROXY_URL = "http://192.168.1.50:8787/usage";   // your PC's LAN IP
const uint32_t POLL_MS   = 12000;    // 拉取间隔(代理已分别缓存：限额180s/细节8s)
const uint32_t RENDER_MS = 5000;     // 检查倒计时变化(只在内容变时才刷出)

// ---------------------- 2) 引脚 ----------------------
#define BL_PIN  5
#define PWR_PIN 7
#define RST_PIN 3
#define BTN_PIN 0      // BOOT 键
Arduino_DataBus* bus = new Arduino_ESP32QSPI(21, 40, 46, 45, 42, 41);
Arduino_GFX* output = new Arduino_ST77916(bus, RST_PIN, 0, true, 360, 360,
                                          0, 0, 0, 0,
                                          st77916_ws_init_operations, sizeof(st77916_ws_init_operations));
// PSRAM 帧缓冲：先画到内存再一次性刷出，消除整屏重绘的闪烁
Arduino_Canvas* canvas = new Arduino_Canvas(360, 360, output);
Arduino_GFX* gfx = canvas;

// ---------------------- 配色 ----------------------
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
const uint16_t C_BG     = RGB565(8, 8, 10);
const uint16_t C_ORANGE = RGB565(244, 168, 54);
const uint16_t C_GLOW   = RGB565(255, 200, 120);
const uint16_t C_DIM    = RGB565(165, 135, 122);
const uint16_t C_BAROFF = RGB565(52, 38, 32);
const uint16_t C_WHITE  = RGB565(238, 232, 226);
const uint16_t C_GREEN  = RGB565(120, 210, 140);
const uint16_t C_AMBER  = RGB565(241, 196, 15);
const uint16_t C_RED    = RGB565(231, 88, 70);

const int CX = 180, CY = 180;

// ---------------------- 数据 ----------------------
struct Usage {
  bool ok = false, stale = false, extraEnabled = false;
  float sessionPct = 0, weekPct = 0, sonnetPct = 0, opusPct = -1, extraPct = -1;
  long  sessionReset = 0, weekReset = 0, activeIdle = -1;
  int64_t tokToday = 0, activeTok = 0;
  int   activeMsgs = 0;
  char  plan[16] = "", activeProj[24] = "";
  uint32_t fetchMs = 0;
} U;
uint32_t lastPoll = 0, lastRender = 0;
int page = 0;
int lastBtn = HIGH; uint32_t lastBtnMs = 0;

// ---------------------- 工具 ----------------------
void txt(const String& s, int x, int y, uint8_t size, uint16_t color) {
  gfx->setFont((const GFXfont*)nullptr);
  gfx->setTextSize(size); gfx->setTextColor(color); gfx->setCursor(x, y); gfx->print(s);
}
int txtW(const String& s, uint8_t size) { return s.length() * 6 * size; }
void logoAt(int x, int y) { gfx->draw16bitRGBBitmap(x, y, (uint16_t*)claude_logo, CLAUDE_LOGO_W, CLAUDE_LOGO_H); }

String humanTok(int64_t t) {
  char b[16];
  if (t >= 1000000) snprintf(b, sizeof(b), "%.1fM", t / 1000000.0);
  else if (t >= 1000) snprintf(b, sizeof(b), "%.0fK", t / 1000.0);
  else snprintf(b, sizeof(b), "%lld", (long long)t);
  return String(b);
}
String fmtReset(long s) {
  char b[16];
  if (s <= 0)         return "now";
  if (s < 3600)       snprintf(b, sizeof(b), "%ldm", s / 60);
  else if (s < 86400) snprintf(b, sizeof(b), "%ldh %ldm", s / 3600, (s % 3600) / 60);
  else                snprintf(b, sizeof(b), "%ldd %ldh", s / 86400, (s % 86400) / 3600);
  return String(b);
}
String fmtIdle(long s) {
  char b[12];
  if (s < 0)    return "?";
  if (s < 60)   snprintf(b, sizeof(b), "%lds", s);
  else if (s < 3600) snprintf(b, sizeof(b), "%ldm", s / 60);
  else          snprintf(b, sizeof(b), "%ldh", s / 3600);
  return String(b);
}
String pctStr(float p) { if (p < 0) return "--"; char b[8]; snprintf(b, sizeof(b), "%.0f%%", p); return String(b); }
uint16_t pctColor(float p) { return p >= 90 ? C_RED : (p >= 70 ? C_AMBER : C_GLOW); }

void segBar(int x0, int y, int w, float frac, uint16_t on, uint16_t off, int n, int h) {
  int gap = 3, segw = (w - (n - 1) * gap) / n;
  frac = constrain(frac, 0.0f, 1.0f);
  int lit = (int)round(frac * n);
  for (int i = 0; i < n; i++) gfx->fillRect(x0 + i * (segw + gap), y, segw, h, i < lit ? on : off);
}
void pageDots(int p) {
  for (int i = 0; i < 2; i++) {
    if (i == p) gfx->fillCircle(CX - 8 + i * 16, 326, 3, C_ORANGE);
    else        gfx->drawCircle(CX - 8 + i * 16, 326, 3, C_DIM);
  }
}

// 计算实时倒计时
long liveReset(long base) { long r = base - (long)((millis() - U.fetchMs) / 1000); return r < 0 ? 0 : r; }

// ---------------------- 电量 / 充电 (BQ27220 @0x55) ----------------------
int  g_bat = -1;
bool g_charging = false;
int  readReg16(uint8_t reg) {                    // 读 2 字节(无符号)，失败返回 -100000
  Wire.beginTransmission(0x55); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -100000;
  if (Wire.requestFrom(0x55, 2) != 2)   return -100000;
  int lo = Wire.read(), hi = Wire.read();
  return (hi << 8) | lo;
}
int readBattery() {                              // StateOfCharge(0x2C, %) —— 电量计已算好，最准
  int v[3], n = 0;
  for (int i = 0; i < 3; i++) { int s = readReg16(0x2C); if (s >= 0 && s <= 100) v[n++] = s; delay(3); }
  if (n == 0) {                                  // SOC 读不到 → 退回电压(0x08)估算
    int mv = readReg16(0x08); if (mv < 2500 || mv > 4500) return g_bat;
    return constrain((int)round((mv - 3300) * 100.0 / 900.0), 0, 100);
  }
  int pct = (n == 3) ? max(min(v[0], v[1]), min(max(v[0], v[1]), v[2]))   // 中位
                     : (n == 2 ? (v[0] + v[1]) / 2 : v[0]);
  if (g_bat >= 0 && abs(pct - g_bat) < 2) return g_bat;   // ±1 抖动不动
  return pct;
}
bool readCharging() {                            // 电流符号优先；≈0 时看 DSG 位(BatteryStatus bit0)
  int cur = readReg16(0x0C);
  if (cur != -100000) { int16_t c = (int16_t)cur; if (c > 5) return true; if (c < -5) return false; }
  int st = readReg16(0x0A);
  if (st != -100000) return (st & 0x0001) == 0; // bit0=DSG，1=放电 → 0=充电/已满
  return g_charging;
}
void drawBolt(Arduino_GFX* g, int x, int y, uint16_t c) {   // ~6x11 小闪电
  g->fillTriangle(x + 4, y,     x,     y + 6, x + 3, y + 6,  c);
  g->fillTriangle(x + 3, y + 5, x + 6, y + 5, x + 2, y + 11, c);
}
void drawBattery(int x, int y, int pct, bool charging) {    // %文字 + 电池框 + 充电闪电
  if (pct < 0) return;
  char b[6]; snprintf(b, sizeof(b), "%d%%", pct);
  txt(b, x - 6 - txtW(b, 2), y - 2, 2, C_DIM);
  gfx->drawRoundRect(x, y, 24, 12, 2, C_DIM);
  gfx->fillRect(x + 24, y + 3, 3, 6, C_DIM);     // 正极头
  int fw = (int)round(pct / 100.0 * 20);
  uint16_t fc = charging ? C_GREEN : (pct <= 15 ? C_RED : (pct <= 30 ? C_AMBER : C_ORANGE));
  if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, 8, fc);
  if (charging) drawBolt(gfx, x + 9, y, C_WHITE);
}
// 充电时电池条"上涨扫描"动画：只把电池小区域直接画到 output(不触发整屏 flush)，所以不闪
void drawChargeFrame(int x, int y, int pct, int level) {
  output->fillRect(x, y, 28, 13, C_BG);
  output->drawRoundRect(x, y, 24, 12, 2, C_DIM);
  output->fillRect(x + 24, y + 3, 3, 6, C_DIM);
  if (level > 0) output->fillRect(x + 2, y + 2, min(level, 20), 8, C_GREEN);
  drawBolt(output, x + 9, y, C_WHITE);
}

// ---------------------- 触摸滑动(CST816S @0x15) ----------------------
#define TOUCH_ADDR 0x15
bool touchActive = false; int swStartX = 0, swStartY = 0, swCurX = 0, swCurY = 0; uint32_t swStart = 0;
bool readTouchRaw(uint8_t& fingers, int& x, int& y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write((uint8_t)0x02);                       // 从 finger 数寄存器开始
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 5) != 5) return false;
  fingers = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return true;
}
// 滑动完成返回 true（任意方向，位移>50；规避触摸坐标轴可能的旋转/镜像）
bool checkSwipe() {
  uint8_t fn = 0; int x = 0, y = 0;
  if (!readTouchRaw(fn, x, y)) fn = 0;             // 休眠/NACK 当作无触摸
  uint32_t now = millis();
  if (fn > 0) {
    if (!touchActive) { touchActive = true; swStartX = x; swStartY = y; swStart = now; }
    swCurX = x; swCurY = y;
    return false;
  }
  if (touchActive) {
    touchActive = false;
    int dx = swCurX - swStartX, dy = swCurY - swStartY;
    if (now - swStart < 900 && max(abs(dx), abs(dy)) > 50) return true;
  }
  return false;
}

// ---------------------- Page 1：概览 ----------------------
void gauge(int y, const String& label, float pct, long resetS) {
  uint16_t pc = pctColor(pct);
  txt(label, 52, y, 2, C_WHITE);
  String ps = pctStr(pct);
  txt(ps, 308 - txtW(ps, 4), y - 8, 4, pc);
  segBar(52, y + 34, 256, pct / 100.0, pc, C_BAROFF, 22, 14);
  gfx->fillCircle(56, y + 62, 3, C_DIM);
  txt(fmtReset(resetS), 66, y + 56, 2, C_DIM);
}
void renderPage1() {
  gfx->fillScreen(C_BG);
  String plan = String(U.plan); plan.toUpperCase();
  int gw = CLAUDE_LOGO_W + 8 + txtW("CLAUDE", 3);
  int sx0 = CX - gw / 2;
  logoAt(sx0, 26);
  txt("CLAUDE", sx0 + CLAUDE_LOGO_W + 8, 38, 3, C_WHITE);
  if (plan.length()) txt(plan, CX - txtW(plan, 2) / 2, 76, 2, C_ORANGE);

  if (!U.ok && U.sessionPct == 0 && U.weekPct == 0) {
    txt("NO DATA", CX - txtW("NO DATA", 3) / 2, 175, 3, C_RED);
    txt("check proxy / login", CX - txtW("check proxy / login", 2) / 2, 210, 2, C_DIM);
    pageDots(page); return;
  }
  gauge(124, "SESSION", U.sessionPct, liveReset(U.sessionReset));
  gauge(212, "WEEKLY",  U.weekPct,    liveReset(U.weekReset));

  uint16_t dot = !U.ok ? C_RED : (U.stale ? C_AMBER : C_GREEN);
  gfx->fillCircle(60, 301, 5, dot);
  txt("CLAUDE CODE", 74, 295, 2, U.stale ? C_AMBER : C_DIM);
  drawBattery(290, 289, g_bat, g_charging);     // 电量挪到右下角，避免与顶部重叠
  pageDots(page);
}

// ---------------------- Page 2：细节 ----------------------
void limRow(int y, const String& k, float pct, long resetS) {
  txt(k, 44, y, 2, C_DIM);
  txt(pctStr(pct), 150, y, 2, pct < 0 ? C_DIM : pctColor(pct));
  if (resetS >= 0) { String r = fmtReset(resetS); txt(r, 312 - txtW(r, 2), y, 2, C_DIM); }
}
void renderPage2() {
  gfx->fillScreen(C_BG);
  int gw = CLAUDE_LOGO_W + 8 + txtW("DETAILS", 3);   // 居中，避免 logo 被圆角裁掉
  int tx = CX - gw / 2;
  logoAt(tx, 34);
  txt("DETAILS", tx + CLAUDE_LOGO_W + 8, 46, 3, C_WHITE);
  drawBattery(290, 289, g_bat, g_charging);     // 与第1页一致，挪到右下角避免压标题

  if (!U.ok && U.tokToday == 0) {
    txt("NO DATA", CX - txtW("NO DATA", 3) / 2, 175, 3, C_RED);
    pageDots(page); return;
  }

  limRow(92,  "5H",     U.sessionPct, liveReset(U.sessionReset));
  limRow(120, "WEEK",   U.weekPct,    liveReset(U.weekReset));
  // SONNET + OPUS 同行
  txt("SONNET", 44, 148, 2, C_DIM);  txt(pctStr(U.sonnetPct), 150, 148, 2, U.sonnetPct < 0 ? C_DIM : pctColor(U.sonnetPct));
  txt("OPUS",   210, 148, 2, C_DIM); txt(pctStr(U.opusPct),   282, 148, 2, U.opusPct < 0 ? C_DIM : pctColor(U.opusPct));
  txt("EXTRA",  44, 176, 2, C_DIM);  txt(U.extraEnabled ? (U.extraPct < 0 ? "on" : pctStr(U.extraPct)) : "off", 150, 176, 2, C_DIM);

  gfx->drawFastHLine(44, 202, 272, C_BAROFF);

  // 今日 token
  txt("TODAY", 44, 214, 2, C_DIM);
  txt(humanTok(U.tokToday) + " tok", 312 - txtW(humanTok(U.tokToday) + " tok", 2), 214, 2, C_GLOW);

  // 当前活跃会话
  txt("ACTIVE", 44, 244, 2, C_DIM);
  txt(String(U.activeProj), 150, 244, 2, C_WHITE);
  String d = humanTok(U.activeTok) + "  " + String(U.activeMsgs) + " msg  idle " + fmtIdle(U.activeIdle);
  txt(d, CX - txtW(d, 1) / 2, 274, 1, C_DIM);

  uint16_t dot = !U.ok ? C_RED : (U.stale ? C_AMBER : C_GREEN);
  gfx->fillCircle(CX - txtW("LIVE", 1) / 2 - 8, 297, 4, dot);
  txt("LIVE", CX - txtW("LIVE", 1) / 2, 294, 1, C_DIM);
  pageDots(page);
}

// 内容签名(分钟级；不含秒级 idle)——只在内容变化时才刷出，消除周期性闪屏
String lastSig = "";
String computeSig() {
  String s = String(page) + (U.ok ? "1" : "0") + (U.stale ? "1" : "0") + String(g_bat) + (g_charging ? "C" : "c");
  s += pctStr(U.sessionPct) + pctStr(U.weekPct)
     + fmtReset(liveReset(U.sessionReset)) + fmtReset(liveReset(U.weekReset));
  if (page == 1)
    s += pctStr(U.sonnetPct) + pctStr(U.opusPct) + (U.extraEnabled ? "E" : "e")
       + humanTok(U.tokToday) + String(U.activeProj) + String(U.activeMsgs);
  return s;
}
void render() {
  String sig = computeSig();
  if (sig == lastSig) return;                    // 没变就不重画、不刷出(不闪)
  lastSig = sig;
  if (page == 0) renderPage1(); else renderPage2();
  canvas->flush();
}

// ---------------------- 拉数据 ----------------------
bool poll() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(8000); http.setTimeout(8000);
  if (!http.begin(PROXY_URL)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); U.ok = false; return false; }
  String payload = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { U.ok = false; return false; }
  U.ok           = doc["ok"] | false;
  U.stale        = doc["stale"] | false;
  U.sessionPct   = doc["session_pct"] | 0.0;
  U.sessionReset = doc["session_reset_s"] | 0L;
  U.weekPct      = doc["week_pct"] | 0.0;
  U.weekReset    = doc["week_reset_s"] | 0L;
  U.sonnetPct    = doc["sonnet_pct"] | -1.0;
  U.opusPct      = doc["opus_pct"] | -1.0;
  U.extraPct     = doc["extra_pct"] | -1.0;
  U.extraEnabled = doc["extra_enabled"] | false;
  U.tokToday     = doc["tok_today"].as<int64_t>();
  U.activeTok    = doc["active_tok"].as<int64_t>();
  U.activeMsgs   = doc["active_msgs"] | 0;
  U.activeIdle   = doc["active_idle_s"] | -1L;
  strlcpy(U.plan, doc["plan"] | "", sizeof(U.plan));
  strlcpy(U.activeProj, doc["active_project"] | "?", sizeof(U.activeProj));
  U.fetchMs = millis();
  g_bat = readBattery();
  g_charging = readCharging();
  return true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  gfx->fillScreen(C_BG);
  logoAt(CX - CLAUDE_LOGO_W / 2, 132);
  txt("connecting wifi...", CX - txtW("connecting wifi...", 2) / 2, 195, 2, C_DIM);
  canvas->flush();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
}

// ---------------------- setup / loop ----------------------
void setup() {
  Serial.begin(115200);
  pinMode(PWR_PIN, OUTPUT); digitalWrite(PWR_PIN, HIGH);
  pinMode(BL_PIN, OUTPUT);  digitalWrite(BL_PIN, HIGH);
  pinMode(BTN_PIN, INPUT_PULLUP);
  Wire.begin(11, 10); Wire.setClock(400000);    // 触摸 + 电量 I2C
  g_bat = readBattery();
  g_charging = readCharging();
  bool cok = gfx->begin(40000000);
  Serial.printf("canvas begin=%d  (0=帧缓冲分配失败,需 PSRAM=opi)\n", cok);
  gfx->fillScreen(C_BG);
  logoAt(CX - CLAUDE_LOGO_W / 2, 120);
  txt("CLAUDE", CX - txtW("CLAUDE", 3) / 2, 180, 3, C_WHITE);
  canvas->flush();

  connectWiFi();
  poll();
  render();
  lastPoll = lastRender = millis();
}

void loop() {
  uint32_t now = millis();

  // BOOT 键翻页(按下=LOW，去抖)
  int b = digitalRead(BTN_PIN);
  if (lastBtn == HIGH && b == LOW && now - lastBtnMs > 250) {
    page = (page + 1) % 2; lastBtnMs = now; render(); lastRender = now;
  }
  lastBtn = b;

  // 触摸滑动翻页
  if (checkSwipe()) { page = (page + 1) % 2; render(); lastRender = now; }

  if (now - lastPoll >= POLL_MS) {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    poll();
    render();
    lastPoll = lastRender = now;
  } else if (now - lastRender >= RENDER_MS) {
    render();
    lastRender = now;
  }

  // 充电状态每秒复查一次(插拔即时反映)
  static uint32_t lastChg = 0, lastAnim = 0; static int animLevel = 0;
  if (now - lastChg >= 1000) {
    bool c = readCharging();
    if (c != g_charging) { g_charging = c; render(); lastRender = now; animLevel = 0; }
    lastChg = now;
  }
  // 充电时电池条上涨扫描动画(只直接画电池小区域，不整屏刷)
  if (g_charging && g_bat >= 0 && now - lastAnim >= 90) {
    int full = (int)round(g_bat / 100.0 * 20);
    animLevel += 2; if (animLevel > full + 3) animLevel = 0;
    drawChargeFrame(290, 289, g_bat, animLevel);
    lastAnim = now;
  }

  delay(20);
}
