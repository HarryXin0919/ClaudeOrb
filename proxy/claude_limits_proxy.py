#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Claude 用量代理 (订阅版, 双页) —— 给 ESP32-S3 圆屏喂数据。

第1页(限额)：读本机 OAuth token 调 /api/oauth/usage，拿"套餐用量限额"
              (会话5h% / 周全模型% / 周Opus% / 周Sonnet% / 额外用量) + 重置时间。
第2页(细节)：读本机 Claude Code 日志(~/.claude/projects)，算今日消耗 token +
              当前活跃会话(项目 / 该会话 token / 空闲秒 / 消息数)。

两块分别缓存：限额走 OAuth(默认180s，别太勤会429)；细节读本地日志(默认8s，可勤)。
都不进固件、不出局域网。

跑法: python claude_limits_proxy.py    ->  ESP32 GET http://<本机IP>:8787/usage
环境变量: PORT / CACHE_TTL(限额) / DET_TTL(细节) / UPSTREAM_PROXY / CC_UA / CRED_FILE / CLAUDE_PROJECTS_DIR / PROXY_TOKEN
"""
import os
import re
import json
import time
import glob
import threading
import datetime as dt
import urllib.request
import urllib.parse
import urllib.error
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

CRED_FILE    = os.environ.get("CRED_FILE", os.path.expanduser("~/.claude/.credentials.json"))
PROJECTS_DIR = os.environ.get("CLAUDE_PROJECTS_DIR", os.path.expanduser("~/.claude/projects"))
PORT         = int(os.environ.get("PORT", "8787"))
DISC_PORT    = int(os.environ.get("DISC_PORT", "8788"))   # UDP 自动发现端口(0=关闭)
SHARED_TOKEN = os.environ.get("PROXY_TOKEN", "").strip()
LIM_TTL      = max(60, int(os.environ.get("CACHE_TTL", "180")))
DET_TTL      = max(3,  int(os.environ.get("DET_TTL", "8")))
CC_UA        = os.environ.get("CC_UA", "claude-code/1.0.0")

UPSTREAM_PROXY = os.environ.get("UPSTREAM_PROXY", "http://127.0.0.1:7890").strip()
_opener = urllib.request.build_opener(
    urllib.request.ProxyHandler({"http": UPSTREAM_PROXY, "https": UPSTREAM_PROXY})) if UPSTREAM_PROXY \
    else urllib.request.build_opener()

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
_lim = {"ts": 0.0, "data": None}
_det = {"ts": 0.0, "data": None}
_hist = {"ts": 0.0, "data": None}
_hist_busy = False
HIST_TTL = max(60, int(os.environ.get("HIST_TTL", "300")))   # 30天扫描较重，缓存久一点

# 天气 (Open-Meteo, 免 key)，给 RLCD-4.2 用
WX_LAT  = float(os.environ.get("WX_LAT", "31.2304"))    # 上海
WX_LON  = float(os.environ.get("WX_LON", "121.4737"))
WX_CITY = os.environ.get("WX_CITY", "Shanghai")
WX_TZ   = os.environ.get("WX_TZ", "Asia/Shanghai")
WX_TTL  = max(300, int(os.environ.get("WX_TTL", "1800")))
_wx = {"ts": 0.0, "data": None}
_wx_busy = False
_direct = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # 直连(不走 UPSTREAM_PROXY)


def _safe_err(msg):
    """返回给客户端的错误信息脱敏:本地文件路径(含用户名)只留给控制台,不出网。"""
    msg = re.sub(r"[A-Za-z]:\\[^\s'\"]+", "<path>", str(msg))
    return re.sub(r"(?:/[\w.~-]+){2,}", "<path>", msg)[:140]


# ---------------- 限额 (OAuth) ----------------
def _read_token():
    with open(CRED_FILE, "r", encoding="utf-8") as f:
        c = json.load(f)
    o = c.get("claudeAiOauth") or c.get("claude_ai_oauth") or {}
    return o.get("accessToken") or o.get("access_token"), o.get("subscriptionType", "")


def _reset_s(iso):
    try:
        d = dt.datetime.fromisoformat(str(iso).replace("Z", "+00:00"))
        return max(0, int((d - dt.datetime.now(dt.timezone.utc)).total_seconds()))
    except Exception:
        return 0


def _pct(node):
    try:
        v = (node or {}).get("utilization")
        return round(float(v), 1) if v is not None else -1.0
    except Exception:
        return -1.0


def fetch_limits():
    tok, sub = _read_token()
    if not tok:
        return {"ok": False, "err": "no oauth token (credentials.json)"}
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": "Bearer " + tok,
        "anthropic-beta": "oauth-2025-04-20",
        "User-Agent": CC_UA,
        "Content-Type": "application/json",
    })
    with _opener.open(req, timeout=20) as r:
        j = json.loads(r.read().decode("utf-8"))
    fh = j.get("five_hour") or {}
    sd = j.get("seven_day") or {}
    so = j.get("seven_day_sonnet")
    op = j.get("seven_day_opus")
    ex = j.get("extra_usage") or {}
    return {
        "ok": True,
        "session_pct": _pct(fh), "session_reset_s": _reset_s(fh.get("resets_at", "")),
        "week_pct": _pct(sd),    "week_reset_s": _reset_s(sd.get("resets_at", "")),
        "sonnet_pct": _pct(so),
        "sonnet_reset_s": _reset_s((so or {}).get("resets_at", "")),
        "opus_pct": _pct(op),
        "opus_reset_s": _reset_s((op or {}).get("resets_at", "")),
        "extra_enabled": bool(ex.get("is_enabled")),
        "extra_pct": _pct(ex),
        "extra_reset_s": _reset_s(ex.get("resets_at", "")),
        "plan": sub,
    }


def cached_limits():
    nowt = time.time()
    if _lim["data"] and _lim["data"].get("ok") and nowt - _lim["ts"] < LIM_TTL:
        return _lim["data"]
    try:
        d = fetch_limits()
        if d.get("ok"):
            _lim["data"], _lim["ts"] = d, nowt
            return d
        if _lim["data"]:
            s = dict(_lim["data"]); s["stale"] = True; s["err"] = d.get("err"); return s
        return d
    except urllib.error.HTTPError as e:
        msg = f"HTTP {e.code}: " + e.read().decode('utf-8', 'ignore')[:120]
        print("[ERR limits]", msg)
        msg = _safe_err(msg)
        if _lim["data"]:
            s = dict(_lim["data"]); s["stale"] = True; s["err"] = msg; return s
        return {"ok": False, "err": msg}
    except Exception as e:  # noqa: BLE001
        print("[ERR limits]", str(e)[:140])
        msg = _safe_err(e)
        if _lim["data"]:
            s = dict(_lim["data"]); s["stale"] = True; s["err"] = msg; return s
        return {"ok": False, "err": msg}


# ---------------- 细节 (本地日志) ----------------
def _local(iso):
    try:
        d = dt.datetime.fromisoformat(str(iso).replace("Z", "+00:00"))
        if d.tzinfo is None:
            d = d.replace(tzinfo=dt.timezone.utc)
        return d.astimezone()
    except Exception:
        return None


def _usage_in(u):
    return (int(u.get("input_tokens", 0) or 0)
            + int(u.get("cache_read_input_tokens", 0) or 0)
            + int(u.get("cache_creation_input_tokens", 0) or 0))


# 每百万 token 估算单价(USD)，仅用于"今日花费"粗估展示 (input, cache_read, cache_write, output)
_RATES = {"opus": (5.0, 0.50, 6.25, 25.0), "sonnet": (3.0, 0.30, 3.75, 15.0), "haiku": (1.0, 0.10, 1.25, 5.0)}


def _rate_for(model):
    m = (model or "").lower()
    if "opus" in m:  return _RATES["opus"]
    if "haiku" in m: return _RATES["haiku"]
    return _RATES["sonnet"]            # 默认按 sonnet


def _cost(model, u):
    ri, rcr, rcw, ro = _rate_for(model)
    return (int(u.get("input_tokens", 0) or 0) * ri
            + int(u.get("cache_read_input_tokens", 0) or 0) * rcr
            + int(u.get("cache_creation_input_tokens", 0) or 0) * rcw
            + int(u.get("output_tokens", 0) or 0) * ro) / 1e6


def _parse_active(fp, now):
    cwd = ""; tok = 0; msgs = 0; last = None; seen = set()
    try:
        with open(fp, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                if ('"cwd"' not in line) and ('"usage"' not in line):
                    continue
                try:
                    o = json.loads(line)
                except Exception:
                    continue
                if not cwd and o.get("cwd"):
                    cwd = o.get("cwd")
                msg = o.get("message") or {}
                if msg.get("role") != "assistant":
                    continue
                u = msg.get("usage") or {}
                if not u:
                    continue
                mid = msg.get("id")
                if mid:
                    if mid in seen:
                        continue
                    seen.add(mid)
                tok += _usage_in(u) + int(u.get("output_tokens", 0) or 0)
                msgs += 1
                d = _local(o.get("timestamp", ""))
                if d and (last is None or d > last):
                    last = d
    except Exception:
        pass
    proj = os.path.basename(cwd.rstrip("\\/")) if cwd else os.path.basename(os.path.dirname(fp))
    idle = int((now - last).total_seconds()) if last else -1
    return {"active_project": proj or "?", "active_tok": tok, "active_msgs": msgs, "active_idle_s": idle}


def scan_details():
    now = dt.datetime.now().astimezone()
    today0 = now.replace(hour=0, minute=0, second=0, microsecond=0)
    cutoff = today0.timestamp() - 3600
    in_t = out_t = 0; cost = 0.0; seen = set()
    files = glob.glob(os.path.join(PROJECTS_DIR, "**", "*.jsonl"), recursive=True)
    for fp in files:
        try:
            if os.path.getmtime(fp) < cutoff:
                continue
        except OSError:
            continue
        try:
            with open(fp, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if '"usage"' not in line:
                        continue
                    try:
                        o = json.loads(line)
                    except Exception:
                        continue
                    msg = o.get("message") or {}
                    if msg.get("role") != "assistant":
                        continue
                    u = msg.get("usage") or {}
                    if not u:
                        continue
                    mid = msg.get("id")
                    if mid:
                        if mid in seen:
                            continue
                        seen.add(mid)
                    d = _local(o.get("timestamp", ""))
                    if not d or d < today0:
                        continue
                    in_t += _usage_in(u)
                    out_t += int(u.get("output_tokens", 0) or 0)
                    cost += _cost(msg.get("model", ""), u)
        except Exception:
            continue
    active = {"active_project": "?", "active_tok": 0, "active_msgs": 0, "active_idle_s": -1}
    if files:
        try:
            active = _parse_active(max(files, key=os.path.getmtime), now)
        except Exception:
            pass
    return {"tok_today_in": in_t, "tok_today_out": out_t, "tok_today": in_t + out_t,
            "cost_today_usd": round(cost, 2), **active}


def cached_details():
    nowt = time.time()
    if _det["data"] and nowt - _det["ts"] < DET_TTL:
        return _det["data"]
    try:
        _det["data"], _det["ts"] = scan_details(), nowt
    except Exception as e:  # noqa: BLE001
        print("[ERR details]", str(e)[:140])
        if not _det["data"]:
            _det["data"] = {"tok_today": 0, "active_project": "?", "active_tok": 0, "active_idle_s": -1}
    return _det["data"]


# ---------------- 7天柱图 + 周/月花费 (本地日志，扫30天) ----------------
def scan_history():
    now = dt.datetime.now().astimezone()
    today0 = now.replace(hour=0, minute=0, second=0, microsecond=0)
    start7  = today0 - dt.timedelta(days=6)          # 含今天共 7 天(柱图)
    start30 = today0 - dt.timedelta(days=29)         # 含今天共 30 天(月花费)
    cutoff = start30.timestamp() - 3600
    buckets = {}; cost7 = 0.0; cost30 = 0.0; seen = set()
    for fp in glob.glob(os.path.join(PROJECTS_DIR, "**", "*.jsonl"), recursive=True):
        try:
            if os.path.getmtime(fp) < cutoff:
                continue
        except OSError:
            continue
        try:
            with open(fp, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if '"usage"' not in line:
                        continue
                    try:
                        o = json.loads(line)
                    except Exception:
                        continue
                    msg = o.get("message") or {}
                    if msg.get("role") != "assistant":
                        continue
                    u = msg.get("usage") or {}
                    if not u:
                        continue
                    mid = msg.get("id")
                    if mid:
                        if mid in seen:
                            continue
                        seen.add(mid)
                    d = _local(o.get("timestamp", ""))
                    if not d or d < start30:
                        continue
                    c = _cost(msg.get("model", ""), u)
                    cost30 += c
                    if d >= start7:
                        key = d.date().isoformat()
                        buckets[key] = buckets.get(key, 0) + _usage_in(u) + int(u.get("output_tokens", 0) or 0)
                        cost7 += c
        except Exception:
            continue
    tok_7d = [int(buckets.get((start7 + dt.timedelta(days=i)).date().isoformat(), 0)) for i in range(7)]
    return {"tok_7d": tok_7d, "week_tok": int(sum(tok_7d)),
            "cost_7d_usd": round(cost7, 2), "cost_30d_usd": round(cost30, 2)}


def _hist_compute():
    global _hist_busy
    try:
        _hist["data"], _hist["ts"] = scan_history(), time.time()
    except Exception as e:  # noqa: BLE001
        print("[ERR history]", str(e)[:140])
    finally:
        _hist_busy = False


def cached_history():
    global _hist_busy
    nowt = time.time()
    if _hist["data"] is None:                         # 首次:同步跑一次(启动已预热则不会到这)
        if not _hist_busy:
            _hist_busy = True
            _hist_compute()
        return _hist["data"] or {"tok_7d": [0] * 7, "week_tok": 0, "cost_7d_usd": 0, "cost_30d_usd": 0}
    if nowt - _hist["ts"] >= HIST_TTL and not _hist_busy:   # 过期 → 后台刷新，不阻塞 ESP
        _hist_busy = True
        threading.Thread(target=_hist_compute, daemon=True).start()
    return _hist["data"]


# ---------------- 天气 (Open-Meteo) ----------------
def fetch_weather():
    url = ("https://api.open-meteo.com/v1/forecast"
           "?latitude=%s&longitude=%s"
           "&current=temperature_2m,weather_code,relative_humidity_2m,apparent_temperature,wind_speed_10m"
           "&hourly=temperature_2m,precipitation_probability"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&timezone=%s&forecast_days=5"
           % (WX_LAT, WX_LON, urllib.parse.quote(WX_TZ)))
    req = urllib.request.Request(url, headers={"User-Agent": "ClaudeOrb-RLCD/1.0"})
    try:
        with _opener.open(req, timeout=15) as r:
            j = json.loads(r.read().decode("utf-8"))
    except Exception:
        # UPSTREAM_PROXY 的节点连不上 open-meteo 时直连兜底(国内可直连)
        with _direct.open(req, timeout=15) as r:
            j = json.loads(r.read().decode("utf-8"))
    cur = j.get("current") or {}
    d = j.get("daily") or {}
    times = d.get("time", []); codes = d.get("weather_code", [])
    his = d.get("temperature_2m_max", []); los = d.get("temperature_2m_min", [])
    days = []
    for i in range(min(5, len(times))):
        try:
            dow = dt.datetime.fromisoformat(times[i]).strftime("%a")
        except Exception:
            dow = ""
        days.append({"d": dow, "c": int(codes[i]),
                     "hi": int(round(his[i])), "lo": int(round(los[i]))})
    # 逐时:从当前整点起取 12 小时
    hr = j.get("hourly") or {}
    htimes = hr.get("time", []); htemps = hr.get("temperature_2m", []); hprec = hr.get("precipitation_probability", [])
    h0 = 0
    try:
        nowh = dt.datetime.now().replace(minute=0, second=0, microsecond=0)
        for i, ts in enumerate(htimes):
            if dt.datetime.fromisoformat(ts) >= nowh:
                h0 = i; break
    except Exception:
        h0 = 0
    hourly_12h = [{"t": int(round(htemps[i])),
                   "p": int(hprec[i]) if i < len(hprec) and hprec[i] is not None else 0}
                  for i in range(h0, min(h0 + 12, len(htemps)))]
    return {"now_t": int(round(cur.get("temperature_2m", 0))),
            "now_c": int(cur.get("weather_code", 0)),
            "now_feels": int(round(cur.get("apparent_temperature", cur.get("temperature_2m", 0)))),
            "now_hum": int(round(cur.get("relative_humidity_2m", 0))),
            "now_wind": int(round(cur.get("wind_speed_10m", 0))),
            "city": WX_CITY, "days": days, "hourly_12h": hourly_12h}


def _wx_compute():
    global _wx_busy
    try:
        _wx["data"], _wx["ts"] = fetch_weather(), time.time()
    except Exception as e:  # noqa: BLE001
        print("[ERR weather]", str(e)[:120])
        _wx["ts"] = time.time() - WX_TTL + 120   # 失败退避:2 分钟后再试
    finally:
        _wx_busy = False


def cached_weather():
    """天气只在后台线程刷新,请求路径绝不阻塞(上游挂死曾拖到 20s+,ESP32 直接超时 NO DATA)。"""
    global _wx_busy
    if time.time() - _wx["ts"] >= WX_TTL and not _wx_busy:
        _wx_busy = True
        threading.Thread(target=_wx_compute, daemon=True).start()
    return _wx["data"]


def payload():
    p = dict(cached_limits())
    p.update(cached_details())
    p.update(cached_history())
    wx = cached_weather()
    if wx:
        p["weather"] = wx
    p["ts"] = int(time.time())
    return p


# ---------------- HTTP ----------------
class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authed(self, query):
        if not SHARED_TOKEN:
            return True
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer ") and auth[7:].strip() == SHARED_TOKEN:
            return True
        q = urllib.parse.parse_qs(query)
        return q.get("token", [""])[0] == SHARED_TOKEN

    def do_GET(self):  # noqa: N802
        u = urllib.parse.urlparse(self.path)
        if u.path not in ("/usage", "/"):
            return self._send(404, {"ok": False, "err": "not found"})
        if not self._authed(u.query):
            return self._send(401, {"ok": False, "err": "bad token"})
        self._send(200, payload())

    def log_message(self, *a):
        pass


# ---------------- UDP 自动发现 ----------------
# 屏在局域网广播 CLAUDE_DASH_DISCOVER_V1 → 这里应答 CLAUDE_DASH_PROXY_V1 <HTTP端口>,
# 屏取应答包的源 IP 当 proxy 地址。电脑 IP 变了/换了网络都不用重烧固件。
def _discovery_loop():
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", DISC_PORT))
    resp = f"CLAUDE_DASH_PROXY_V1 {PORT}".encode("ascii")
    while True:
        try:
            data, addr = s.recvfrom(64)
            if data.strip() == b"CLAUDE_DASH_DISCOVER_V1":
                s.sendto(resp, addr)
        except Exception:
            time.sleep(0.5)


if __name__ == "__main__":
    print(f"Claude 用量代理(双页) → http://0.0.0.0:{PORT}/usage  (UA={CC_UA}, 限额{LIM_TTL}s 细节{DET_TTL}s)")
    print(f"  凭据: {CRED_FILE}")
    print(f"  日志: {PROJECTS_DIR}")
    if DISC_PORT:
        print(f"  发现: UDP {DISC_PORT}")
        threading.Thread(target=_discovery_loop, daemon=True).start()
    _hist_busy = True                                # 启动即后台预热 30 天扫描
    threading.Thread(target=_hist_compute, daemon=True).start()
    _wx_busy = True                                  # 天气也预热,且永不阻塞请求
    threading.Thread(target=_wx_compute, daemon=True).start()
    try:
        ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
