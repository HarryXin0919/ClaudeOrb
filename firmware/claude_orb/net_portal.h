/* ============================================================================
   net_portal.h —— WiFi 配网门户 + proxy UDP 自动发现 (ClaudeOrb / ClaudeSlate 共用)

   换网络不用重烧固件:
   · WiFi 凭据 / proxy 地址存 NVS(Preferences),编译期常量只做首次出厂默认
   · 开机连不上 WiFi → 自动变成热点(NET_AP_NAME),手机连上后弹出配网页
     (或手动开 http://192.168.4.1),填新 WiFi 保存即重启生效;
     5 分钟没人配置会自动重启重试——防止路由器比屏开机慢时卡死在配网模式
   · proxy 地址不写死:向局域网广播 CLAUDE_DASH_DISCOVER_V1(UDP 8788),
     proxy 应答 CLAUDE_DASH_PROXY_V1 <端口>,取应答源 IP,电脑 IP 变了自动跟上

   用法(include 前先定义):
     #define NET_DEF_SSID       "..."        // 出厂默认 WiFi
     #define NET_DEF_PASS       "..."
     #define NET_DEF_PROXY_HOST "192.168.x.x"  // 发现失败时的兜底地址
     #define NET_DEF_PROXY_PORT 8787
     #define NET_AP_NAME        "ClaudeSlate-Setup"
   ============================================================================ */
#pragma once
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ---- WPA2/WPA3-Enterprise(学校/公司网络:同时要"用户名+密码")----
// PEAP/TTLS 这类企业网络不是一个 PSK 密码能连的,要 identity+username+password 走 EAP。
// 凭据 API 名字在 Arduino core 3.x(IDF5)是 esp_eap_client_*,2.x(IDF4)是 esp_wifi_sta_wpa2_ent_*。
#if __has_include(<esp_eap_client.h>)
  #include <esp_eap_client.h>
  #define ORB_EAP(fn)       esp_eap_client_##fn
  #define ORB_EAP_ENABLE()  esp_wifi_sta_enterprise_enable()
#else
  #include <esp_wpa2.h>
  #define ORB_EAP(fn)       esp_wifi_sta_wpa2_ent_##fn
  #define ORB_EAP_ENABLE()  esp_wifi_sta_wpa2_ent_enable()
#endif

#define NET_DISC_PORT  8788
#define NET_DISC_REQ   "CLAUDE_DASH_DISCOVER_V1"
#define NET_DISC_RESP  "CLAUDE_DASH_PROXY_V1"
#define NET_PORTAL_TIMEOUT_MS 300000UL
#ifndef NET_PROXY_QUERY
#define NET_PROXY_QUERY ""   // 编译期兜底 token;现在优先用配网门户里填的运行期 token(见 netCfg.proxyTok)
#endif
#ifndef NET_DEF_PROXY_TOK
#define NET_DEF_PROXY_TOK ""   // 出厂默认 proxy token(留空=不带);可在配网门户填,改 token 免重烧
#endif
#ifndef NET_DEF_USER
#define NET_DEF_USER ""   // 出厂默认非企业网(家用 WiFi 只用密码);填了用户名才走 EAP 企业网
#endif

struct NetConfig {
  String   ssid, pass, user, proxyHost, proxyTok;   // user 非空 = 企业网(用户名+密码)
  uint16_t proxyPort;
};
NetConfig netCfg;

// 配网期间禁止后台 pollTask 碰 WiFi:门户要在 loop 上下文重配 WiFi/扫描,而 pollTask 在
// 核0 跑 HTTP/discovery,两个任务同时驱动 esp-idf WiFi 驱动会崩(LoadProhibited)或卡死。
// pollTask 进循环先把网络段标记 busy 再复检 g_inPortal;门户置位后等 busy 落下即可独占 WiFi。
volatile bool g_inPortal = false;   // 门户已接管 WiFi(置位后不再清,直到重启)
volatile bool g_pollBusy = false;   // pollTask 正在跑 poll()/netDiscoverProxy()

void netBegin() {
  Preferences p; p.begin("netcfg", true);
  netCfg.ssid      = p.getString("ssid",  NET_DEF_SSID);
  netCfg.pass      = p.getString("pass",  NET_DEF_PASS);
  netCfg.user      = p.getString("user",  NET_DEF_USER);
  netCfg.proxyHost = p.getString("phost", NET_DEF_PROXY_HOST);
  netCfg.proxyPort = p.getUShort("pport", NET_DEF_PROXY_PORT);
  netCfg.proxyTok  = p.getString("ptok",  NET_DEF_PROXY_TOK);
  p.end();
}

void netSaveWiFi(const String& ssid, const String& pass, const String& user) {
  Preferences p; p.begin("netcfg", false);
  p.putString("ssid", ssid); p.putString("pass", pass); p.putString("user", user);
  p.end();
  netCfg.ssid = ssid; netCfg.pass = pass; netCfg.user = user;
}

void netSaveProxy(const String& host, uint16_t port) {
  Preferences p; p.begin("netcfg", false);
  p.putString("phost", host); p.putUShort("pport", port);
  p.end();
  netCfg.proxyHost = host; netCfg.proxyPort = port;
}

void netSaveToken(const String& tok) {
  Preferences p; p.begin("netcfg", false);
  p.putString("ptok", tok);
  p.end();
  netCfg.proxyTok = tok;
}

String netProxyUrl() {
  // 运行期 token 优先(配网门户里填的);留空才回落到编译期 NET_PROXY_QUERY
  String q = netCfg.proxyTok.length() ? ("?token=" + netCfg.proxyTok) : String(NET_PROXY_QUERY);
  return "http://" + netCfg.proxyHost + ":" + String(netCfg.proxyPort) + "/usage" + q;
}

bool netConnect(uint32_t timeoutMs = 20000) {
  WiFi.mode(WIFI_STA);
  if (netCfg.user.length()) {                        // 企业网(用户名+密码,如学校 eduroam / hopkins)
    // PEAP/MSCHAPv2:identity 用用户名(外层匿名身份),username/password 为内层凭据。
    // 默认不校验服务器证书(没传 CA),换学校网零配置即连;netConnect 每次开机只跑一次,无需 disable。
    ORB_EAP(set_identity)((const uint8_t*)netCfg.user.c_str(), netCfg.user.length());
    ORB_EAP(set_username)((const uint8_t*)netCfg.user.c_str(), netCfg.user.length());
    ORB_EAP(set_password)((const uint8_t*)netCfg.pass.c_str(), netCfg.pass.length());
    ORB_EAP_ENABLE();
    WiFi.begin(netCfg.ssid.c_str());                 // 企业网不传 PSK,凭据走上面的 EAP 配置
  } else {
    WiFi.begin(netCfg.ssid.c_str(), netCfg.pass.c_str());
  }
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

// 广播找 proxy;找到则更新 NVS 缓存。失败保持原地址(可能 proxy 没开机)。
bool netDiscoverProxy(int tries = 3) {
  if (WiFi.status() != WL_CONNECTED) return false;
  IPAddress ip = WiFi.localIP(), mask = WiFi.subnetMask(), bc;
  for (int i = 0; i < 4; i++) bc[i] = ip[i] | ~mask[i];
  WiFiUDP udp;
  if (!udp.begin(48788)) return false;
  bool found = false;
  for (int t = 0; t < tries && !found; t++) {
    udp.beginPacket(bc, NET_DISC_PORT);
    udp.write((const uint8_t*)NET_DISC_REQ, strlen(NET_DISC_REQ));
    udp.endPacket();
    uint32_t t0 = millis();
    while (millis() - t0 < 1200) {
      if (udp.parsePacket() > 0) {
        char buf[64] = {0};
        int n = udp.read(buf, sizeof(buf) - 1);
        if (n > 0 && strncmp(buf, NET_DISC_RESP, strlen(NET_DISC_RESP)) == 0) {
          uint16_t port = atoi(buf + strlen(NET_DISC_RESP));
          String host = udp.remoteIP().toString();
          if (port == 0) port = NET_DEF_PROXY_PORT;
          if (host != netCfg.proxyHost || port != netCfg.proxyPort) netSaveProxy(host, port);
          found = true; break;
        }
      }
      delay(20);
    }
  }
  udp.stop();
  return found;
}

// ---------------------- 配网门户 ----------------------
static String _netHtmlEscape(const String& s) {
  String o; o.reserve(s.length());
  for (char c : s) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

// ---------------------- WiFi 扫描(多遍累积 + 去重 + 按信号排序)----------------------
// 为什么以前"扫不到所有网络":①单遍扫描会漏掉 beacon 间隔长/信号弱的 AP;②旧代码 i<12
// 截的是"原始结果下标",mesh 路由同名 SSID 占多个 BSSID 会把 12 格挤满、真网络排在后面被砍;
// ③没按 RSSI 排序、每信道驻留太短。本实现:多遍主动扫描累积、按 SSID 去重(留最强信号)、
// 按信号从强到弱排序、加大展示上限、带信号条。注:ESP32-S3 只有 2.4GHz 射频,5GHz 网络
// 物理上扫不到也连不上——页面已注明,免得用户一直找一个永远出不来的网络。
#ifndef NET_SCAN_PASSES
#define NET_SCAN_PASSES      2      // 扫描遍数(累积去重),漏网时可调到 3
#endif
#ifndef NET_SCAN_MS_PER_CHAN
#define NET_SCAN_MS_PER_CHAN 300    // 每信道驻留 ms(默认 ~120 太短),主动扫描
#endif
#ifndef NET_SCAN_MAX_SHOW
#define NET_SCAN_MAX_SHOW    30      // 去重排序后最多展示数(旧值仅 12)
#endif
#ifndef NET_SCAN_MAX_UNIQUE
#define NET_SCAN_MAX_UNIQUE  48      // 累积容器上限
#endif

static String _netScanOpts;         // 缓存:去重排序后的 <option> 列表
static int    _netScanCount = 0;     // 去重后发现的网络数(可能 > 展示数)
static int    _netScanShown = 0;     // 实际进下拉的条数(= <option> value 的索引范围)
static String _netSsid[NET_SCAN_MAX_UNIQUE];  // 原始 SSID 字节,/save 按索引精确取回——
                                              // 解决"中文名乱码且选了连不上"(GBK 路由 vs UTF-8 页面)
static String _netEntJs;            // 企业网(需用户名)的下拉索引列表(JS 数组串),选中即自动展开用户名框

// 企业网络(WPA2/WPA3-Enterprise,PEAP/TTLS):要 identity+username+password,一个 PSK 连不上。
// 学校/公司网(eduroam、hopkins 等)就是这类——单凭密码框无法连,门户需额外要用户名。
static bool _netIsEnt(wifi_auth_mode_t m) {
  return m == WIFI_AUTH_WPA2_ENTERPRISE
      || m == WIFI_AUTH_WPA3_ENTERPRISE
      || m == WIFI_AUTH_WPA2_WPA3_ENTERPRISE;
}

// 多遍扫描,结果累积进 _netScanOpts / _netScanCount。需在 STA 或 APSTA 模式下调用。
static void _netScan() {
  // 用文件级 _netSsid[](/save 要按索引取回原始字节);rs/op 仍 static 挪出 8KB loop 栈
  String*        ss = _netSsid;
  static int32_t rs[NET_SCAN_MAX_UNIQUE];
  static bool    op[NET_SCAN_MAX_UNIQUE];
  static bool    en[NET_SCAN_MAX_UNIQUE];            // 企业网标记,与 ss/rs 同索引
  int cnt = 0;
  for (int pass = 0; pass < NET_SCAN_PASSES; pass++) {
    // (async=false, show_hidden=true, passive=false=主动扫描, max_ms_per_chan)
    int n = WiFi.scanNetworks(false, true, false, NET_SCAN_MS_PER_CHAN);
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;                 // 隐藏网络无 SSID → 走手动输入
      int32_t r = WiFi.RSSI(i);
      wifi_auth_mode_t m = WiFi.encryptionType(i);
      bool open = (m == WIFI_AUTH_OPEN || m == WIFI_AUTH_OWE);   // OWE=增强开放,用户视角也是开放
      bool ent  = _netIsEnt(m);                                  // 企业网(需用户名+密码)
      int at = -1;
      for (int k = 0; k < cnt; k++) if (ss[k] == s) { at = k; break; }
      if (at >= 0) { if (r > rs[at]) { rs[at] = r; op[at] = open; en[at] = ent; } }   // 留最强那个
      else if (cnt < NET_SCAN_MAX_UNIQUE) { ss[cnt] = s; rs[cnt] = r; op[cnt] = open; en[cnt] = ent; cnt++; }
    }
    WiFi.scanDelete();
    if (pass + 1 < NET_SCAN_PASSES) delay(120);
  }
  // 按 RSSI 从强到弱插入排序
  for (int i = 1; i < cnt; i++) {
    String s = ss[i]; int32_t r = rs[i]; bool o = op[i]; bool e = en[i]; int j = i - 1;
    while (j >= 0 && rs[j] < r) { ss[j+1]=ss[j]; rs[j+1]=rs[j]; op[j+1]=op[j]; en[j+1]=en[j]; j--; }
    ss[j+1]=s; rs[j+1]=r; op[j+1]=o; en[j+1]=e;
  }
  String opts, enjs;
  int shown = (cnt < NET_SCAN_MAX_SHOW) ? cnt : NET_SCAN_MAX_SHOW;
  opts.reserve(shown * 96);                          // 一次性预留,免 30 次 += 反复重分配
  for (int i = 0; i < shown; i++) {
    // value=索引(不是 SSID 字节):GBK 中文名在 UTF-8 页里会被浏览器改写成 U+FFFD,
    // 用索引提交、固件按 _netSsid[i] 取原始字节去连,乱码名也能连上。
    // 名字保持干净(不加信号条方块字符——在手机上会渲染成莫名其妙的横线);
    // 列表已按信号从强到弱排序,自家网通常就在最上面。
    opts += "<option value=\"" + String(i) + "\">" + _netHtmlEscape(ss[i]);
    if (op[i])      opts += " &middot; open";        // 开放网
    else if (en[i]) { opts += " &middot; login";     // 企业网:标记 + 记下索引给前端 JS 自动展开用户名框
                      if (enjs.length()) enjs += ","; enjs += String(i); }
    opts += "</option>";
  }
  _netScanOpts  = opts;
  _netEntJs     = enjs;
  _netScanCount = cnt;
  _netScanShown = shown;
}

// 阻塞式:保存后或超时即重启,不返回调用方继续跑
void netStartPortal() {
  g_inPortal = true;                                 // 接管 WiFi:挡住后台 pollTask 的新一轮拉取
  uint32_t gw = millis();                            // 等当前在飞的 poll()(HTTP 最长 ~8s)收尾
  while (g_pollBusy && millis() - gw < 9000) delay(20);
  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(150);
  _netScan();                                        // 先扫(多遍)再开 AP,结果嵌进页面
  WiFi.mode(WIFI_AP);
  WiFi.softAP(NET_AP_NAME);
  delay(200);

  DNSServer dns; dns.start(53, "*", WiFi.softAPIP());
  WebServer srv(80);

  // 页面动态生成:rescan 后 _netScanOpts/_netScanCount 变了要反映出来
  auto buildPage = [&]() -> String {
    String found = (_netScanCount > 0)
        ? (String(_netScanCount) + " network" + (_netScanCount == 1 ? "" : "s") + " found"
           + (_netScanCount > NET_SCAN_MAX_SHOW ? (", top " + String(NET_SCAN_MAX_SHOW)) : ""))
        : "none found &mdash; tap rescan or type manually";
    return String(
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>Claude Dash Setup</title><style>"
      "body{font-family:system-ui;background:#16120e;color:#eee8e2;margin:0;padding:24px}"
      ".card{max-width:420px;margin:auto}h1{font-size:20px;color:#f4a836}"
      "label{display:block;margin:14px 0 6px;font-size:13px;color:#a58772}"
      "select,input{width:100%;padding:10px;border-radius:8px;border:1px solid #5a4636;"
      "background:#241c15;color:#eee8e2;font-size:15px;box-sizing:border-box}"
      "button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:8px;"
      "background:#f4a836;color:#1a140e;font-size:16px;font-weight:700}"
      ".hint{font-size:12px;color:#7a6552;margin-top:6px}"
      ".row{display:flex;align-items:center;justify-content:space-between}"
      ".rescan{font-size:12px;color:#f4a836;text-decoration:none;border:1px solid #5a4636;"
      "padding:4px 10px;border-radius:6px}"
      "</style></head><body><div class=card><h1>Claude Dash &middot; WiFi</h1>"
      "<form method=POST action=/save>"
      "<div class=row><label>WiFi &middot; ") + found + "</label>"
      "<a class=rescan href=/rescan>&#8635; rescan</a></div>"
      "<select name=ssid>"
      + _netScanOpts +
      "<option value=\"\">manual input...</option></select>"
      "<input id=m name=ssid2 placeholder=SSID style=\"display:none;margin-top:8px\">"
      "<div id=urow style=\"display:none\">"
      "<label>Username <span style=\"color:#f4a836\">&middot; school / work login</span></label>"
      "<input name=user autocapitalize=none autocorrect=off spellcheck=false "
      "placeholder=\"e.g. you@jh.edu\"></div>"
      "<label>Password</label><input name=pass type=password>"
      "<label>Proxy (optional)</label><input name=proxy placeholder=\"empty = auto discover\">"
      "<label>Proxy token (optional)</label>"
      "<input name=ptok autocapitalize=none autocorrect=off spellcheck=false "
      "placeholder=\"match proxy PROXY_TOKEN; blank = keep current\">"
      "<div class=hint>Set this if the proxy runs with PROXY_TOKEN (needed when it binds "
      "0.0.0.0 for LAN). Mismatch &rarr; the orb shows NO PROXY.</div>"
      "<div class=hint>School / company WiFi that asks for a login = pick the &middot; login one, "
      "then fill Username + Password (PEAP). A hidden enterprise SSID: type it manually &mdash; "
      "the username box appears for manual entries too.</div>"
      "<div class=hint>Only 2.4GHz networks are shown &mdash; this board has no 5GHz radio, "
      "so a 5GHz-only SSID can't be seen or joined.</div>"
      "<div class=hint>A garbled name = router broadcasts it in GBK, not UTF-8. "
      "Just pick it anyway &mdash; it still connects (list is sorted by signal, yours is usually on top).</div>"
      "<div class=hint>saved &rarr; device reboots onto the new network</div>"
      "<button>Save &amp; Reboot</button></form></div>"
      // 进页/重扫后按当前选中项同步手动输入框可见性——只靠 onchange 在"仅手动一项"时不触发,
      // 会把扫不到网络的用户卡死(无法输 SSID)。这段在 load 时跑一次补上。
      "<script>var EN=[" + _netEntJs + "];(function(){"
      "var s=document.querySelector('select[name=ssid]'),"
      "m=document.getElementById('m'),ur=document.getElementById('urow');"
      "function u(){var man=!s.value;m.style.display=man?'block':'none';"      // 选了"手动输入"才显示 SSID 框
      "ur.style.display=(man||EN.indexOf(parseInt(s.value,10))>=0)?'block':'none';}"  // 企业网或手动 → 显示用户名框
      "s.addEventListener('change',u);u();})();</script>"
      "</body></html>";
  };

  srv.on("/", [&]() { srv.send(200, "text/html", buildPage()); });
  srv.on("/rescan", [&]() {                           // 手动重扫:APSTA 下扫,AP 不掉线
    WiFi.mode(WIFI_AP_STA);
    _netScan();
    WiFi.mode(WIFI_AP);
    srv.sendHeader("Location", "/", true);
    srv.send(303, "text/plain", "");                  // 303 → 浏览器 GET /
  });
  srv.on("/save", HTTP_POST, [&]() {
    String sel = srv.arg("ssid"), ssid;
    bool isIdx = sel.length() > 0;                   // 下拉项 value 是纯数字索引;手动项 value=""
    for (uint16_t k = 0; k < sel.length(); k++) if (sel[k] < '0' || sel[k] > '9') { isIdx = false; break; }
    if (isIdx) {                                     // 选了下拉:按索引取扫描时存的原始字节(乱码也精确)
      int idx = sel.toInt();
      if (idx >= 0 && idx < _netScanShown) ssid = _netSsid[idx];
    }
    if (!ssid.length()) { ssid = srv.arg("ssid2"); ssid.trim(); }   // 手动框:去首尾空白再判空
    String pass = srv.arg("pass"), proxy = srv.arg("proxy");
    String user = srv.arg("user"); user.trim();      // 企业网用户名;留空=普通 PSK 网络(走纯密码)
    if (ssid.length()) netSaveWiFi(ssid, pass, user);
    proxy.trim();
    if (proxy.length()) {                            // 手填 host[:port];留空走自动发现
      int c = proxy.indexOf(':');
      if (c > 0) netSaveProxy(proxy.substring(0, c), (uint16_t)proxy.substring(c + 1).toInt());
      else       netSaveProxy(proxy, NET_DEF_PROXY_PORT);
    }
    String ptok = srv.arg("ptok"); ptok.trim();      // proxy token:填了才存,留空=保留原值
    if (ptok.length()) netSaveToken(ptok);
    srv.send(200, "text/html", "<body style=\"background:#16120e;color:#eee8e2;"
             "font-family:system-ui;text-align:center;padding-top:40vh\">saved, rebooting...</body>");
    delay(1500);
    ESP.restart();
  });
  srv.onNotFound([&]() {                             // 捕获系统的联网检测请求,弹出配网页
    srv.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    srv.send(302, "text/plain", "");
  });
  srv.begin();

  uint32_t t0 = millis();
  while (millis() - t0 < NET_PORTAL_TIMEOUT_MS) {
    dns.processNextRequest();
    srv.handleClient();
    delay(5);
  }
  ESP.restart();
}
