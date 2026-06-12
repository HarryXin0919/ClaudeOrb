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

#define NET_DISC_PORT  8788
#define NET_DISC_REQ   "CLAUDE_DASH_DISCOVER_V1"
#define NET_DISC_RESP  "CLAUDE_DASH_PROXY_V1"
#define NET_PORTAL_TIMEOUT_MS 300000UL
#ifndef NET_PROXY_QUERY
#define NET_PROXY_QUERY ""   // proxy 设了 PROXY_TOKEN 时改成 "?token=<密码>"
#endif

struct NetConfig {
  String   ssid, pass, proxyHost;
  uint16_t proxyPort;
};
NetConfig netCfg;

void netBegin() {
  Preferences p; p.begin("netcfg", true);
  netCfg.ssid      = p.getString("ssid",  NET_DEF_SSID);
  netCfg.pass      = p.getString("pass",  NET_DEF_PASS);
  netCfg.proxyHost = p.getString("phost", NET_DEF_PROXY_HOST);
  netCfg.proxyPort = p.getUShort("pport", NET_DEF_PROXY_PORT);
  p.end();
}

void netSaveWiFi(const String& ssid, const String& pass) {
  Preferences p; p.begin("netcfg", false);
  p.putString("ssid", ssid); p.putString("pass", pass);
  p.end();
  netCfg.ssid = ssid; netCfg.pass = pass;
}

void netSaveProxy(const String& host, uint16_t port) {
  Preferences p; p.begin("netcfg", false);
  p.putString("phost", host); p.putUShort("pport", port);
  p.end();
  netCfg.proxyHost = host; netCfg.proxyPort = port;
}

String netProxyUrl() {
  return "http://" + netCfg.proxyHost + ":" + String(netCfg.proxyPort) + "/usage" NET_PROXY_QUERY;
}

bool netConnect(uint32_t timeoutMs = 20000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(netCfg.ssid.c_str(), netCfg.pass.c_str());
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

// 阻塞式:保存后或超时即重启,不返回调用方继续跑
void netStartPortal() {
  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
  int n = WiFi.scanNetworks();                       // 先扫再开 AP,扫描结果嵌进页面
  String opts;
  for (int i = 0; i < n && i < 12; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || opts.indexOf(">" + _netHtmlEscape(s) + "<") >= 0) continue;
    opts += "<option>" + _netHtmlEscape(s) + "</option>";
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(NET_AP_NAME);
  delay(200);

  DNSServer dns; dns.start(53, "*", WiFi.softAPIP());
  WebServer srv(80);

  String page =
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
    "</style></head><body><div class=card><h1>Claude Dash &middot; WiFi</h1>"
    "<form method=POST action=/save>"
    "<label>WiFi</label>"
    "<select name=ssid onchange=\"document.getElementById('m').style.display=this.value?'none':'block'\">"
    + opts +
    "<option value=\"\">manual input...</option></select>"
    "<input id=m name=ssid2 placeholder=SSID style=\"display:none;margin-top:8px\">"
    "<label>Password</label><input name=pass type=password>"
    "<label>Proxy (optional)</label><input name=proxy placeholder=\"empty = auto discover\">"
    "<div class=hint>saved &rarr; device reboots onto the new network</div>"
    "<button>Save &amp; Reboot</button></form></div></body></html>";

  srv.on("/", [&]() { srv.send(200, "text/html", page); });
  srv.on("/save", HTTP_POST, [&]() {
    String ssid = srv.arg("ssid"); if (!ssid.length()) ssid = srv.arg("ssid2");
    String pass = srv.arg("pass"), proxy = srv.arg("proxy");
    if (ssid.length()) netSaveWiFi(ssid, pass);
    proxy.trim();
    if (proxy.length()) {                            // 手填 host[:port];留空走自动发现
      int c = proxy.indexOf(':');
      if (c > 0) netSaveProxy(proxy.substring(0, c), (uint16_t)proxy.substring(c + 1).toInt());
      else       netSaveProxy(proxy, NET_DEF_PROXY_PORT);
    }
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
