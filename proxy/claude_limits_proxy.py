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
import json
import time
import glob
import datetime as dt
import urllib.request
import urllib.parse
import urllib.error
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

CRED_FILE    = os.environ.get("CRED_FILE", os.path.expanduser("~/.claude/.credentials.json"))
PROJECTS_DIR = os.environ.get("CLAUDE_PROJECTS_DIR", os.path.expanduser("~/.claude/projects"))
PORT         = int(os.environ.get("PORT", "8787"))
SHARED_TOKEN = os.environ.get("PROXY_TOKEN", "").strip()
LIM_TTL      = max(60, int(os.environ.get("CACHE_TTL", "180")))
DET_TTL      = max(3,  int(os.environ.get("DET_TTL", "8")))
CC_UA        = os.environ.get("CC_UA", "claude-code/1.0.0")

# Outbound proxy for api.anthropic.com. Empty = direct (default).
# If your network can't reach Anthropic directly (e.g. mainland China), set e.g.
#   UPSTREAM_PROXY=http://127.0.0.1:7890   (a local Clash/HTTP proxy)
UPSTREAM_PROXY = os.environ.get("UPSTREAM_PROXY", "").strip()
_opener = urllib.request.build_opener(
    urllib.request.ProxyHandler({"http": UPSTREAM_PROXY, "https": UPSTREAM_PROXY})) if UPSTREAM_PROXY \
    else urllib.request.build_opener()

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
_lim = {"ts": 0.0, "data": None}
_det = {"ts": 0.0, "data": None}


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
        "opus_pct": _pct(op),
        "extra_enabled": bool(ex.get("is_enabled")),
        "extra_pct": _pct(ex),
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
        if _lim["data"]:
            s = dict(_lim["data"]); s["stale"] = True; s["err"] = msg; return s
        return {"ok": False, "err": msg}
    except Exception as e:  # noqa: BLE001
        msg = str(e)[:140]
        print("[ERR limits]", msg)
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
    in_t = out_t = 0; seen = set()
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
        except Exception:
            continue
    active = {"active_project": "?", "active_tok": 0, "active_msgs": 0, "active_idle_s": -1}
    if files:
        try:
            active = _parse_active(max(files, key=os.path.getmtime), now)
        except Exception:
            pass
    return {"tok_today_in": in_t, "tok_today_out": out_t, "tok_today": in_t + out_t, **active}


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


def payload():
    p = dict(cached_limits())
    p.update(cached_details())
    p["ts"] = int(time.time())
    return p


# ---------------- HTTP ----------------
class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        u = urllib.parse.urlparse(self.path)
        if u.path not in ("/usage", "/"):
            return self._send(404, {"ok": False, "err": "not found"})
        if SHARED_TOKEN:
            q = urllib.parse.parse_qs(u.query)
            if q.get("token", [""])[0] != SHARED_TOKEN:
                return self._send(401, {"ok": False, "err": "bad token"})
        self._send(200, payload())

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    print(f"Claude 用量代理(双页) → http://0.0.0.0:{PORT}/usage  (UA={CC_UA}, 限额{LIM_TTL}s 细节{DET_TTL}s)")
    print(f"  凭据: {CRED_FILE}")
    print(f"  日志: {PROJECTS_DIR}")
    try:
        ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
