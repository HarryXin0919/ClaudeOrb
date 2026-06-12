/* ============================================================================
   ClaudeOrb —— phone-style launcher on Waveshare ESP32-S3-Touch-LCD-1.85B (ST77916 360x360)
   Home: app icon grid (Claude / Clock / Weather / Setup) + status bar (time, battery).
   Tap an icon to open an app; BOOT (GPIO0) = home button; hold BOOT 3s = WiFi portal.
   - Claude:  usage dashboard, 2 pages (swipe to flip)
   - Clock:   big digital clock w/ seconds, SESSION usage ring + seconds sweep on bezel
   - Weather: now + 5-day forecast   - Setup: network info
   Data from claude_limits_proxy.py; polling runs on a background task (core 0).

   FQBN: esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=16M,PSRAM=opi
   (PSRAM=opi 给 Arduino_Canvas 帧缓冲；电量来自 BQ27220 @0x55)
   ============================================================================ */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include "st77916_ws_init.h"
#include "claude_logo.h"

// ---------------------- 1) EDIT THESE / 改这里 ----------------------
// Factory defaults only — runtime config lives in NVS. If WiFi fails, the
// device opens a setup hotspot (captive portal) so you never need to reflash;
// the proxy is found via UDP broadcast, so its IP may change freely.
#define NET_DEF_SSID       "YOUR_WIFI_SSID"     // 2.4GHz only (ESP32-S3 can't do 5GHz)
#define NET_DEF_PASS       "YOUR_WIFI_PASSWORD"
#define NET_DEF_PROXY_HOST "192.168.1.50"       // fallback when discovery fails
#define NET_DEF_PROXY_PORT 8787
#define NET_AP_NAME        "ClaudeOrb-Setup"    // setup hotspot name
#include "net_portal.h"
const uint32_t POLL_MS   = 12000;    // poll interval (background task; proxy caches: limits 180s / details 8s)
const uint32_t RENDER_MS = 100;      // content-signature check (only flushes on change; clock ticks per second)
const long TZ_OFFSET_S   = 8 * 3600; // your UTC offset in seconds (e.g. UTC+8)
const char* NTP_1 = "pool.ntp.org";
const char* NTP_2 = "ntp.aliyun.com";

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
const uint16_t C_BLUE   = RGB565(92, 156, 255);

const int CX = 180, CY = 180;

// ---------------------- 数据 ----------------------
struct WxDay { char d[6]; int c, hi, lo; };
struct Usage {
  bool ok = false, stale = false, extraEnabled = false;
  float sessionPct = 0, weekPct = 0, sonnetPct = 0, opusPct = -1, extraPct = -1;
  long  sessionReset = 0, weekReset = 0, activeIdle = -1;
  int64_t tokToday = 0, activeTok = 0;
  int   activeMsgs = 0;
  char  plan[16] = "", activeProj[24] = "";
  int   wxT = 0, wxC = -1;           // 当前气温/天气码
  int   wxFeels = 0, wxHum = 0, wxWind = 0;
  WxDay days[5]; int nDays = 0;      // 5天预报(Weather App 用)
  uint32_t fetchMs = 0;
} U;
uint32_t lastPoll = 0, lastRender = 0;
int app = 0;                          // 0=主屏(图标) 1=Claude 2=Clock 3=Weather 4=Setup;BOOT=Home键
int page = 0;
bool timeOK = false;
int lastBtn = HIGH; uint32_t btnDownMs = 0;
volatile uint32_t lastOnline = 0;     // 后台拉取任务写,主循环读
volatile int pollFails = 0;

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
// 手势:0=无 1=轻点(坐标在 tapX/tapY) 2=滑动(任意方向,位移>50;滑动不分方向规避坐标轴旋转/镜像)
int tapX = 0, tapY = 0;
int checkGesture() {
  uint8_t fn = 0; int x = 0, y = 0;
  if (!readTouchRaw(fn, x, y)) fn = 0;             // 休眠/NACK 当作无触摸
  uint32_t now = millis();
  if (fn > 0) {
    if (!touchActive) { touchActive = true; swStartX = x; swStartY = y; swStart = now; }
    swCurX = x; swCurY = y;
    return 0;
  }
  if (touchActive) {
    touchActive = false;
    int dx = swCurX - swStartX, dy = swCurY - swStartY;
    int d = max(abs(dx), abs(dy));
    if (now - swStart < 900 && d > 50) return 2;
    if (now - swStart < 400 && d < 20) { tapX = swStartX; tapY = swStartY; return 1; }
  }
  return 0;
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

// ---------------------- 天气图标/文案(Weather App + 图标网格共用) ----------------------
int wxType(int c) {                              // 0晴 1多云 2阴 3雾 4雨 5雪 6雷
  if (c < 0)  return 2;
  if (c <= 0) return 0;
  if (c <= 2) return 1;
  if (c == 3) return 2;
  if (c == 45 || c == 48) return 3;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return 5;
  if (c >= 95) return 6;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return 4;
  return 2;
}
const char* wxText(int c) {
  switch (wxType(c)) {
    case 0: return "Clear";  case 1: return "P.Cloudy"; case 2: return "Cloudy";
    case 3: return "Fog";    case 4: return "Rain";     case 5: return "Snow";
    default: return "Storm";
  }
}
void wxCloud(int cx, int cy, int s, uint16_t c) {
  gfx->fillCircle(cx - s, cy + s / 2, s * 6 / 10, c);
  gfx->fillCircle(cx + s, cy + s / 2, s * 6 / 10, c);
  gfx->fillCircle(cx, cy - s / 3, s, c);
  gfx->fillRect(cx - s - s * 6 / 10, cy + s / 2 - s * 6 / 10 + 1, (s + s * 6 / 10) * 2, s * 6 / 10, c);
}
void wxSun(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, C_ORANGE);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    gfx->drawLine(cx + (r + 2) * cos(a), cy + (r + 2) * sin(a), cx + (r + 5) * cos(a), cy + (r + 5) * sin(a), C_ORANGE);
  }
}
void drawWxIcon(int cx, int cy, int code, int s) {
  switch (wxType(code)) {
    case 0: wxSun(cx, cy, s); break;
    case 1: wxSun(cx - s * 2 / 3, cy - s / 2, s * 2 / 3); wxCloud(cx + s / 4, cy + s / 4, s * 3 / 4, C_DIM); break;
    case 2: wxCloud(cx, cy, s, C_DIM); break;
    case 3: for (int i = 0; i < 4; i++) gfx->drawFastHLine(cx - s, cy - s / 2 + i * (s / 2), s * 2, C_DIM); break;
    case 4: wxCloud(cx, cy - s / 3, s * 3 / 4, C_DIM);
            for (int i = -1; i <= 1; i++) gfx->drawLine(cx + i * s / 2, cy + s * 2 / 3, cx + i * s / 2 - 2, cy + s, C_BLUE);
            break;
    case 5: wxCloud(cx, cy - s / 3, s * 3 / 4, C_DIM);
            for (int i = -1; i <= 1; i++) gfx->fillCircle(cx + i * s / 2, cy + s * 5 / 6, 2, C_WHITE);
            break;
    case 6: wxCloud(cx, cy - s / 3, s * 3 / 4, C_DIM);
            gfx->fillTriangle(cx - 1, cy + s / 3, cx + 4, cy + s / 3, cx - 4, cy + s, C_AMBER);
            break;
  }
}

// ---------------------- 主屏:App 图标网格(像手机一样,点图标进 App) ----------------------
struct AppIcon { int cx, cy; const char* label; };
const AppIcon ICONS[4] = {{112, 136, "Claude"}, {248, 136, "Clock"}, {112, 252, "Weather"}, {248, 252, "Setup"}};
void drawTile(int cx, int cy) {                  // iOS 风圆角图标底
  gfx->drawRoundRect(cx - 38, cy - 38, 76, 76, 18, C_BAROFF);
  gfx->drawRoundRect(cx - 37, cy - 37, 74, 74, 17, C_BAROFF);
}
void renderHome() {
  gfx->fillScreen(C_BG);
  struct tm t;
  if (getLocalTime(&t, 50)) {                    // 状态栏一行:时间居左 电量居右
    char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    txt(hm, 104, 16, 2, C_WHITE);
  }
  for (int i = 0; i < 4; i++) {
    drawTile(ICONS[i].cx, ICONS[i].cy);
    txt(ICONS[i].label, ICONS[i].cx - txtW(ICONS[i].label, 1) / 2, ICONS[i].cy + 46, 1, C_DIM);
  }
  // Claude 图标:真 logo
  logoAt(ICONS[0].cx - CLAUDE_LOGO_W / 2, ICONS[0].cy - CLAUDE_LOGO_H / 2);
  // Clock 图标:小表盘
  { int cx = ICONS[1].cx, cy = ICONS[1].cy;
    gfx->fillCircle(cx, cy, 24, C_WHITE);
    bool ok = getLocalTime(&t, 20);
    float ah = ((ok ? t.tm_hour % 12 : 10) + (ok ? t.tm_min : 8) / 60.0f) * PI / 6 - PI / 2;
    float am = (ok ? t.tm_min : 8) * PI / 30 - PI / 2;
    gfx->drawLine(cx, cy, cx + (int)(11 * cos(ah)), cy + (int)(11 * sin(ah)), C_BG);
    gfx->drawLine(cx, cy, cx + (int)(17 * cos(am)), cy + (int)(17 * sin(am)), C_BG);
    gfx->fillCircle(cx, cy, 2, C_ORANGE); }
  // Weather 图标:当前天气(无数据画太阳)
  drawWxIcon(ICONS[2].cx, ICONS[2].cy, U.wxC, 14);
  // Setup 图标:齿轮
  { int cx = ICONS[3].cx, cy = ICONS[3].cy;
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4;
      int x1 = cx + (int)(13 * cos(a)), y1 = cy + (int)(13 * sin(a));
      int x2 = cx + (int)(21 * cos(a)), y2 = cy + (int)(21 * sin(a));
      gfx->drawLine(x1, y1, x2, y2, C_DIM); gfx->drawLine(x1 + 1, y1, x2 + 1, y2, C_DIM);
      gfx->drawLine(x1, y1 + 1, x2, y2 + 1, C_DIM); gfx->drawLine(x1 - 1, y1, x2 - 1, y2, C_DIM);
    }
    gfx->fillCircle(cx, cy, 14, C_DIM);
    gfx->fillCircle(cx, cy, 6, C_BG); }
  drawBattery(228, 18, g_bat, g_charging);       // 与时间同行,靠右
}

// ---------------------- Clock App:大数字时钟表盘 ----------------------
const char* WD3[7]  = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
const char* MO3[12] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
// Ring progress bar: continuous round-capped arc from dense dots, clockwise from 12 o'clock
void ringArc(float fromDeg, float toDeg, int r, int dotR, uint16_t color) {
  for (float d = fromDeg; d <= toDeg; d += 1.2f) {
    float a = (d - 90) * DEG_TO_RAD;
    gfx->fillCircle(CX + (int)round(r * cos(a)), CY + (int)round(r * sin(a)), dotR, color);
  }
}
void renderClock() {
  gfx->fillScreen(C_BG);
  // Bezel ring = SESSION usage (clockwise from 12): dim track + lit arc
  ringArc(0, 360, 170, 3, C_BAROFF);
  if (U.ok && U.sessionPct > 0)
    ringArc(0, constrain(U.sessionPct, 0.0f, 100.0f) * 3.6f, 170, 3, pctColor(U.sessionPct));
  logoAt(CX - CLAUDE_LOGO_W / 2, 46);

  struct tm t; bool ok = getLocalTime(&t, 50);   // SNTP 后台同步,成功即显示
  char hm[6] = "--:--", ss[3] = "--";
  if (ok) {
    snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(ss, sizeof(ss), "%02d", t.tm_sec);
  }
  int tw = txtW(hm, 8), sw = txtW(ss, 2);
  int x0 = CX - (tw + 8 + sw) / 2;
  txt(hm, x0, 132, 8, C_WHITE);
  txt(ss, x0 + tw + 8, 180, 2, C_ORANGE);        // 秒,底部对齐
  if (ok) {                                      // seconds: white dot sweeps the bezel (on top of the ring)
    float a = (t.tm_sec * 6 - 90) * DEG_TO_RAD;
    gfx->fillCircle(CX + (int)round(170 * cos(a)), CY + (int)round(170 * sin(a)), 5, C_WHITE);
  }
  if (ok) {
    char ds[16]; snprintf(ds, sizeof(ds), "%s %s %d", WD3[t.tm_wday], MO3[t.tm_mon], t.tm_mday);
    txt(ds, CX - txtW(ds, 2) / 2, 218, 2, C_ORANGE);
  }
  if (U.wxC >= 0) {
    char ts[8]; snprintf(ts, sizeof(ts), "%d", U.wxT);
    int w = txtW(ts, 2);
    txt(ts, CX - (w + 8) / 2, 248, 2, C_DIM);
    gfx->drawCircle(CX - (w + 8) / 2 + w + 4, 250, 2, C_DIM);
  }
  drawBattery(290, 289, g_bat, g_charging);
}

// ---------------------- Weather App:当前 + 5天预报 ----------------------
void renderWeather() {
  gfx->fillScreen(C_BG);
  txt("WEATHER", CX - txtW("WEATHER", 2) / 2, 18, 2, C_DIM);
  if (U.wxC < 0) {
    txt("NO DATA", CX - txtW("NO DATA", 3) / 2, 170, 3, C_RED);
    return;
  }
  drawWxIcon(105, 84, U.wxC, 16);
  char ts[8]; snprintf(ts, sizeof(ts), "%d", U.wxT);
  txt(ts, 150, 56, 7, C_WHITE);
  gfx->drawCircle(150 + txtW(ts, 7) + 8, 62, 4, C_WHITE);
  txt(wxText(U.wxC), CX - txtW(wxText(U.wxC), 2) / 2, 122, 2, C_GLOW);
  char m[44]; snprintf(m, sizeof(m), "feels %d  hum %d%%  wind %dkm/h", U.wxFeels, U.wxHum, U.wxWind);
  txt(m, CX - txtW(m, 1) / 2, 148, 1, C_DIM);
  gfx->drawFastHLine(60, 164, 240, C_BAROFF);
  for (int i = 0; i < U.nDays && i < 5; i++) {
    int y = 178 + i * 26;
    txt(U.days[i].d, 70, y, 2, C_DIM);
    drawWxIcon(165, y + 6, U.days[i].c, 8);
    char hl[16]; snprintf(hl, sizeof(hl), "%d / %d", U.days[i].hi, U.days[i].lo);
    txt(hl, 290 - txtW(hl, 2), y, 2, C_WHITE);
  }
}

// ---------------------- Setup App:网络信息 ----------------------
void renderSettings() {
  gfx->fillScreen(C_BG);
  txt("SETUP", CX - txtW("SETUP", 2) / 2, 18, 2, C_DIM);
  logoAt(CX - CLAUDE_LOGO_W / 2, 40);
  const char* k[6] = {"WIFI", "IP", "RSSI", "PROXY", "PLAN", "BAT"};
  String v[6] = { netCfg.ssid, WiFi.localIP().toString(), String(WiFi.RSSI()) + " dBm",
                  netCfg.proxyHost, String(U.plan), String(g_bat) + "%" };
  for (int i = 0; i < 6; i++) {
    int y = 96 + i * 28;
    txt(k[i], 70, y, 2, C_DIM);
    txt(v[i], 290 - txtW(v[i], 2), y, 2, C_WHITE);
  }
  txt("hold BOOT 3s = WiFi setup", CX - txtW("hold BOOT 3s = WiFi setup", 1) / 2, 286, 1, C_ORANGE);
}

// 内容签名(分钟级；不含秒级 idle)——只在内容变化时才刷出，消除周期性闪屏
String lastSig = "";
String computeSig() {
  struct tm t; bool ok = getLocalTime(&t, 20);
  String mn = String(ok ? t.tm_hour * 60 + t.tm_min : -1);
  String bt = String(g_bat) + (g_charging ? "C" : "c");
  if (app == 0)                                  // 主屏:分钟 + 电量 + 天气图标
    return "L" + mn + bt + String(U.wxC);
  if (app == 2)                                  // Clock:秒级 + 电量 + 用量环 + 气温
    return "K" + mn + ":" + String(ok ? t.tm_sec : -1) + bt
         + (U.ok ? pctStr(U.sessionPct) : "-") + String(U.wxC >= 0 ? U.wxT : -99);
  if (app == 3) {                                // Weather:天气字段
    String s = "W" + bt + String(U.wxT) + String(U.wxC) + String(U.wxHum) + String(U.wxWind);
    for (int i = 0; i < U.nDays; i++) s += String(U.days[i].c) + String(U.days[i].hi) + String(U.days[i].lo);
    return s;
  }
  if (app == 4)                                  // Setup:网络状态(RSSI 量化防闪)
    return "S" + bt + netCfg.ssid + WiFi.localIP().toString() + String(WiFi.RSSI() / 5) + netCfg.proxyHost;
  String s = "C" + String(page) + (U.ok ? "1" : "0") + (U.stale ? "1" : "0") + bt;
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
  if (app == 0)      renderHome();
  else if (app == 2) renderClock();
  else if (app == 3) renderWeather();
  else if (app == 4) renderSettings();
  else if (page == 0) renderPage1();
  else                renderPage2();
  canvas->flush();
}

// ---------------------- 拉数据 ----------------------
bool poll() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(8000); http.setTimeout(8000);
  if (!http.begin(netProxyUrl())) return false;
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
  JsonObject wx = doc["weather"];
  if (!wx.isNull()) {
    U.wxT = wx["now_t"] | 0; U.wxC = wx["now_c"] | -1;
    U.wxFeels = wx["now_feels"] | U.wxT; U.wxHum = wx["now_hum"] | 0; U.wxWind = wx["now_wind"] | 0;
    JsonArray ds = wx["days"]; U.nDays = 0;
    for (JsonObject d : ds) {
      if (U.nDays >= 5) break;
      strlcpy(U.days[U.nDays].d, d["d"] | "", sizeof(U.days[0].d));
      U.days[U.nDays].c = d["c"] | 0; U.days[U.nDays].hi = d["hi"] | 0; U.days[U.nDays].lo = d["lo"] | 0;
      U.nDays++;
    }
  }
  U.fetchMs = millis();
  return true;
}

// 数据拉取后台任务(核0):HTTP 最长阻塞 8 秒,放主循环会卡时钟和触摸。
// I2C(电量/触摸)全部留在主循环,避免跨核抢总线。
void pollTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    if (WiFi.status() != WL_CONNECTED) continue;
    lastOnline = millis();
    if (poll()) pollFails = 0;
    else if (++pollFails >= 3) {                 // 连续拉不到 → proxy IP 可能变了,广播重找
      netDiscoverProxy(2);
      pollFails = 0;
    }
  }
}

void showNetStatus(const char* l1, const char* l2, const char* l3) {
  gfx->fillScreen(C_BG);
  logoAt(CX - CLAUDE_LOGO_W / 2, 96);
  txt(l1, CX - txtW(l1, 2) / 2, 170, 2, C_WHITE);
  if (l2) txt(l2, CX - txtW(l2, 1) / 2, 205, 1, C_DIM);
  if (l3) txt(l3, CX - txtW(l3, 1) / 2, 222, 1, C_DIM);
  canvas->flush();
}

void showSetupScreen() {
  char l2[40]; snprintf(l2, sizeof(l2), "join WiFi: %s", NET_AP_NAME);
  showNetStatus("SETUP MODE", l2, "then open http://192.168.4.1");
}

// 开机版:连不上 → 配网门户(保存或5分钟超时都会重启,不返回)
void connectWiFi() {
  showNetStatus("connecting wifi...", netCfg.ssid.c_str(), nullptr);
  if (netConnect(20000)) { netDiscoverProxy(2); return; }
  showSetupScreen();
  netStartPortal();
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

  netBegin();
  connectWiFi();
  configTime(TZ_OFFSET_S, 0, NTP_1, NTP_2);
  struct tm t; timeOK = getLocalTime(&t, 6000);
  poll();
  render();
  lastPoll = lastRender = millis();
  xTaskCreatePinnedToCore(pollTask, "poll", 12288, nullptr, 1, nullptr, 0);  // 主循环在核1
}

void loop() {
  uint32_t now = millis();

  // BOOT 键 = Home 键(短按回主屏),长按3秒进配网门户
  int b = digitalRead(BTN_PIN);
  if (lastBtn == HIGH && b == LOW) btnDownMs = now;
  if (b == LOW && btnDownMs && now - btnDownMs >= 3000) {
    showSetupScreen();
    netStartPortal();                              // 不返回(保存/超时即重启)
  }
  if (lastBtn == LOW && b == HIGH) {
    if (btnDownMs && now - btnDownMs >= 30 && now - btnDownMs < 1000) {
      app = 0; page = 0;
      render(); lastRender = now;
    }
    btnDownMs = 0;
  }
  lastBtn = b;

  // 触摸:主屏点图标开 App;Claude 里滑动翻页;回主屏按 BOOT
  int gst = checkGesture();
  if (gst) {
    if (app == 0 && gst == 1) {
      for (int i = 0; i < 4; i++)
        if (abs(tapX - ICONS[i].cx) <= 52 && abs(tapY - ICONS[i].cy) <= 56) { app = i + 1; page = 0; break; }
    } else if (app == 1 && gst == 2) {
      page = (page + 1) % 2;
    }
    render(); lastRender = now;
  }

  if (now - lastPoll >= POLL_MS) {               // WiFi 看护(数据拉取在后台任务)
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      if (now - lastOnline > 300000UL) ESP.restart();  // 掉线5分钟重启(连不回去会落入配网门户)
    }
    lastPoll = now;
  }

  if (now - lastRender >= RENDER_MS) {           // 签名没变不会真正刷屏
    render();
    lastRender = now;
  }

  // 电量 10 秒一读;充电状态每秒复查(插拔即时反映)。I2C 只在主循环碰
  static uint32_t lastBat = 0, lastChg = 0, lastAnim = 0; static int animLevel = 0;
  if (now - lastBat >= 10000) { g_bat = readBattery(); lastBat = now; }
  if (now - lastChg >= 1000) {
    bool c = readCharging();
    if (c != g_charging) { g_charging = c; render(); lastRender = now; animLevel = 0; }
    lastChg = now;
  }
  // 充电时电池条上涨扫描动画(只直接画电池小区域，不整屏刷;主屏电量在状态栏)
  if (g_charging && g_bat >= 0 && now - lastAnim >= 90) {
    int full = (int)round(g_bat / 100.0 * 20);
    animLevel += 2; if (animLevel > full + 3) animLevel = 0;
    drawChargeFrame(app == 0 ? 228 : 290, app == 0 ? 18 : 289, g_bat, animLevel);
    lastAnim = now;
  }

  delay(20);
}
