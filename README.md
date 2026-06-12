# ClaudeOrb

**English** | [简体中文](README.zh-CN.md)

A tiny desk **orb that shows your Claude plan usage in real time** — on a 1.85" round
ESP32-S3 display. Glance over and see how much of your current **5‑hour session** and
**weekly** limits you've burned, when they reset, today's token count, and what your
agent is doing right now.

It reads the **same numbers Claude Code shows on its "Plan usage limits" page**
(`Current session 28% · Weekly 12% …`) — straight from your local login, no API key,
no organization account, nothing pasted anywhere.

> Works with a **Claude Pro / Max** subscription used through Claude Code.

```
   ┌─ Home ── app grid ──┐        ┌─ Claude ─ 2 pages ──┐        ┌─ Clock ─────────────┐
   |    21:36   88%[▮▮]  |        |     ✻ CLAUDE        |        |  · usage ring + sec |
   |   ┌────┐   ┌────┐   |  tap   |        MAX          |        |        ✻            |
   |   | ✻  |   | ◷  |   |  --->  | SESSION       29%   |        |    21:36 45         |
   |   └────┘   └────┘   |        | ▮▮▮▮▮▯▯▯▯▯▯▯▯  35m   |  also: |    THU JUN 12       |
   |   Claude   Clock    |        | WEEKLY        12%   |        |       26°           |
   |   ┌────┐   ┌────┐   |  BOOT  | ▮▮▮▯▯▯▯▯▯▯▯▯▯  5d    |  Weather (now + 5-day)     |
   |   | ☼  |   | ⚙  |   |  <---  |   ● CLAUDE CODE     |  Setup (network info)      |
   |   └────┘   └────┘   |        |  swipe to flip page |        |                     |
   |   Weather  Setup    |        └─────────────────────┘        └─────────────────────┘
   └─────────────────────┘
```

---

## How it works

The usage numbers live behind your **Claude OAuth login** (not an `sk-ant-…` key).
A tiny Python **proxy runs on your PC**, reads the OAuth token that Claude Code already
stores locally, calls Anthropic's usage endpoint, and serves a small JSON on your LAN.
The ESP32 just polls that JSON over Wi‑Fi.

```
~/.claude/.credentials.json ──> proxy (your PC) ──(LAN JSON)──> ESP32-S3 round display
   (Claude OAuth token)          api/oauth/usage              ClaudeOrb firmware
```

The token **never leaves your PC** and is **never flashed into the device**.
The proxy also reads your local Claude Code logs (`~/.claude/projects`) for today's
token count and the currently‑active session shown on page 2.

---

## Hardware

A **Waveshare ESP32‑S3‑Touch‑LCD‑1.85** (or 1.85B/1.85C) — a round **360×360** display
with an **ST77916** controller (QSPI) and a **CST816** touch panel. ~$20.

> ⚠️ **Board‑variant gotcha (read this if your screen stays black).**
> The standard 1.85 routes the LCD reset through a **TCA9554 I²C expander** (EXIO2).
> Some units (the one this was built on, silk‑screened **"1.85B"**) have **no expander**
> and wire **LCD_RST to GPIO3** instead. The firmware defaults to **GPIO3**. If your
> screen never lights up, see [Adapting to your board](#adapting-to-your-board).

Pins used by the firmware (QSPI LCD + touch):

| Function | GPIO | Function | GPIO |
|---|---|---|---|
| LCD CS | 21 | LCD D2 | 42 |
| LCD SCK | 40 | LCD D3 | 41 |
| LCD D0 | 46 | **LCD_RST** | **3** (see note) |
| LCD D1 | 45 | Backlight | 5 |
| Power latch | 7 (held high) | Touch I²C | SDA 11 / SCL 10 |
| Touch INT | 4 | BOOT button | 0 |

---

## Setup

### 1) Run the proxy (your PC)

Requires **Python 3.8+** — *zero* third‑party packages. You must already be logged into
**Claude Code** on this machine (`claude` → `/login`).

```bash
python proxy/claude_limits_proxy.py
```

You should see `Claude 用量代理 → http://0.0.0.0:8787/usage`. Open
`http://localhost:8787/usage` in a browser — you should get JSON with `session_pct`,
`week_pct`, `tok_today`, etc.

- No need to look up your PC's IP — the screen **finds the proxy by UDP broadcast**
  (port 8788) and follows it if your PC's IP changes.
- **Allow it through the firewall**: when prompted, let Python accept inbound
  connections (TCP `8787` + UDP `8788`).
- **Can't reach `api.anthropic.com` directly** (e.g. mainland China)? Route the proxy
  through your local HTTP proxy: `UPSTREAM_PROXY=http://127.0.0.1:7890 python proxy/claude_limits_proxy.py`.
- **Security:** the proxy listens on your LAN and never returns your OAuth token —
  only derived percentages. On a shared/untrusted network, set `PROXY_TOKEN=<secret>`
  and set `#define NET_PROXY_QUERY "?token=<secret>"` in the firmware (or send
  `Authorization: Bearer <secret>`); requests without it get 401.

Keep it running. See [Autostart](#autostart-optional) to launch it at login.

### 2) Flash the firmware (ESP32‑S3)

**Arduino IDE / arduino‑cli** with the **esp32 core 3.x**.

1. Install libraries (Library Manager): **GFX Library for Arduino** (by *moononournation*)
   and **ArduinoJson**.
2. Open `firmware/claude_orb/claude_orb.ino`.
3. Optionally edit the `NET_DEF_*` defaults at the top (2.4 GHz WiFi only) — or just
   flash as-is: if the screen can't join WiFi it opens a **setup hotspot**
   (`ClaudeOrb-Setup`); join it from your phone, the config page pops up (or open
   `http://192.168.4.1`), pick your WiFi and save. The proxy is auto-discovered, so
   there's no IP to type. Re-enter setup anytime by **holding BOOT for 3 s**.
4. Board settings:
   - Board: **ESP32S3 Dev Module**
   - **USB CDC On Boot: Enabled**
   - **Partition Scheme: Huge APP (3MB No OTA)**
   - **Flash Size: 16MB**
   - **PSRAM: OPI PSRAM** (used as a frame buffer so the screen never flickers)
5. Upload.

arduino‑cli one‑liner:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=16M,PSRAM=opi \
  -u -p <PORT> firmware/claude_orb
```

> The firmware draws to a PSRAM canvas and only repaints when a value changes
> (flicker‑free), and shows a **battery indicator** in the bottom‑right when a LiPo
> is connected. The charge level comes from the onboard **BQ27220 fuel gauge**
> (StateOfCharge); while charging the gauge turns green with a lightning bolt and an
> animated fill‑sweep, painted directly to that small region so it stays flicker‑free.

After boot it connects to Wi‑Fi and starts polling the proxy. Numbers appear within a
few seconds.

### Autostart (optional)

So the proxy comes back after a reboot:

- **Windows:** copy `proxy/ClaudeUsageProxy.vbs` into your Startup folder
  (`Win+R` → `shell:startup`). It launches the proxy hidden at login. *(Edit the paths
  inside the `.vbs` first.)*
- **macOS/Linux:** run it under `systemd --user`, `launchd`, `pm2`, `nohup`, etc.

---

## Usage

- **Home — app grid:** a phone‑style launcher. Status bar (time + battery) and four
  apps; **tap an icon** to open one, press **BOOT** (the home button) to go back.
- **Claude:** the usage dashboard — page 1 has SESSION / WEEKLY gauges with live reset
  countdowns; page 2 has every limit tier (5H / WEEK / SONNET / OPUS / EXTRA), today's
  token total, and your active Claude Code session. **Swipe** (any direction) to flip.
- **Clock:** big digital clock with ticking seconds, date, current temperature.
  The bezel is a continuous **ring gauge** of SESSION usage (lit clockwise from 12,
  shifting amber/red as you approach the limit); a white dot sweeps the ring with
  the seconds. NTP time (`TZ_OFFSET_S` / `NTP_1` / `NTP_2` in the `.ino`).
- **Weather:** current conditions (feels‑like / humidity / wind) + 5‑day forecast,
  from the proxy's Open‑Meteo data.
- **Setup:** WiFi / IP / RSSI / proxy / plan / battery at a glance.
- Data polling runs on a **background task** (core 0), so the clock and touch never
  stutter while the proxy is fetched.

---

## Customizing

- **Colors / theme:** edit the `C_*` constants near the top of the `.ino`
  (`C_ORANGE` is the accent; default is Claude orange‑yellow `#F4A836`).
- **Logo:** `firmware/claude_orb/claude_logo.h` is generated by `tools/gen_logo.py`
  (`pip install pillow`, then `python tools/gen_logo.py`). Pass `--color R,G,B` to
  recolour it.
- **Refresh rate:** `POLL_MS` (how often the device polls the proxy) and the proxy's
  `CACHE_TTL` (limits, default 180 s — Anthropic throttles this endpoint, don't go lower)
  and `DET_TTL` (local token details, default 8 s).

### Adapting to your board

If the screen stays black, the LCD reset/pins likely differ. The firmware exposes them
at the top of the `.ino` (`RST_PIN`, `BL_PIN`, the `Arduino_ESP32QSPI(...)` pins).
On the **standard 1.85** the reset is behind a **TCA9554 @ 0x20** (set EXIO2 high before
`gfx->begin()`); on the **1.85B** used here it's a plain GPIO (**3**). The panel needs
**Waveshare's ST77916 init sequence** (already bundled as `st77916_ws_init.h`); the
generic init in the GFX library is for a different panel and will glitch.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Screen black | Wrong `RST_PIN` / pins for your board variant — see above. |
| `NO DATA` on screen | Proxy not reachable. Open `http://<PC-IP>:8787/usage` from your phone — if it fails, it's the **firewall** (allow TCP 8787 + UDP 8788 for Python). Discovery retries automatically every ~3 polls. |
| Proxy returns `ok:false` / 401 | Your Claude login/token expired — open Claude Code (it refreshes the token), or `/login` again. |
| Proxy can't reach Anthropic | Set `UPSTREAM_PROXY` (see step 1). |
| Wi‑Fi never connects | It must be **2.4 GHz**. |

---

## Credits & license

- Firmware, proxy and tools: **MIT** (see `LICENSE`).
- ST77916 panel init adapted from Waveshare's ESP32‑S3‑Touch‑LCD‑1.85 demo.
- Display driver: [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX).
- The Claude name and logo are trademarks of **Anthropic**; the logo is rendered only as
  a small on‑device icon for identification. ClaudeOrb is an unofficial, fan‑made project
  and is not affiliated with or endorsed by Anthropic. See `NOTICE`.
