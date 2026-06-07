# ClaudeOrb

A tiny desk **orb that shows your Claude plan usage in real time** — on a 1.85" round
ESP32-S3 display. Glance over and see how much of your current **5‑hour session** and
**weekly** limits you've burned, when they reset, today's token count, and what your
agent is doing right now.

It reads the **same numbers Claude Code shows on its "Plan usage limits" page**
(`Current session 28% · Weekly 12% …`) — straight from your local login, no API key,
no organization account, nothing pasted anywhere.

> Works with a **Claude Pro / Max** subscription used through Claude Code.

```
   ┌─ Page 1 ─ overview ─┐        ┌─ Page 2 ─ details ──┐
   |     ✻ CLAUDE        |        |    ✻ DETAILS        |
   |        MAX          |        | 5H     2%    4h     |
   | SESSION       29%   |  swipe | WEEK   13%   5d     |
   | ▮▮▮▮▮▯▯▯▯▯▯▯▯  35m   |  <-->  | SONNET 0%  OPUS --  |
   | WEEKLY        12%   |  /BOOT | EXTRA  on           |
   | ▮▮▮▯▯▯▯▯▯▯▯▯▯  5d    |        | TODAY     145M tok  |
   |   ● CLAUDE CODE     |        | ACTIVE   my-project |
   └─────────────────────┘        └─────────────────────┘
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

- **Find your PC's LAN IP** (`ipconfig` / `ip addr`) — you'll need it for the firmware.
- **Allow it through the firewall**: when prompted, let Python accept inbound
  connections (it serves on port `8787`).
- **Can't reach `api.anthropic.com` directly** (e.g. mainland China)? Route the proxy
  through your local HTTP proxy: `UPSTREAM_PROXY=http://127.0.0.1:7890 python proxy/claude_limits_proxy.py`.

Keep it running. See [Autostart](#autostart-optional) to launch it at login.

### 2) Flash the firmware (ESP32‑S3)

**Arduino IDE / arduino‑cli** with the **esp32 core 3.x**.

1. Install libraries (Library Manager): **GFX Library for Arduino** (by *moononournation*)
   and **ArduinoJson**.
2. Open `firmware/claude_orb/claude_orb.ino`.
3. Edit the top of the file:
   - `WIFI_SSID` / `WIFI_PASS` — your **2.4 GHz** network (ESP32‑S3 has no 5 GHz).
   - `PROXY_URL` — `http://<your‑PC‑LAN‑IP>:8787/usage`.
4. Board settings:
   - Board: **ESP32S3 Dev Module**
   - **USB CDC On Boot: Enabled**
   - **Partition Scheme: Huge APP (3MB No OTA)**
   - **Flash Size: 16MB**
5. Upload.

arduino‑cli one‑liner:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=16M \
  -u -p <PORT> firmware/claude_orb
```

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

- **Page 1 — overview:** SESSION (5‑hour window) and WEEKLY (all models) as big gauges
  with live reset countdowns.
- **Page 2 — details:** every limit tier (5H / WEEK / SONNET / OPUS / EXTRA), today's
  token total, and your currently‑active Claude Code session (project · tokens · idle).
- **Switch pages:** **swipe** the screen (any direction) or press the **BOOT** button.

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
| `NO DATA` on screen | Proxy not reachable. Open `http://<PC-IP>:8787/usage` from your phone — if it fails, it's the **firewall** or wrong `PROXY_URL`. |
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
