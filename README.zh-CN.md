# ClaudeOrb

[English](README.md) | **简体中文**

一个摆在桌上的**小圆屏摆件,实时显示你的 Claude 套餐用量**——跑在 1.85" 圆形
ESP32-S3 屏上。一眼就能看到当前 **5 小时会话**和**每周**限额用了多少、什么时候重置、
今日 token 数,以及你的 agent 此刻正在干什么。

它读的就是 **Claude Code "Plan usage limits" 页面上的那组数字**
(`Current session 28% · Weekly 12% …`)——直接来自你本机的登录态,**不需要 API key、
不需要组织账户、不用把任何东西贴到别处**。

> 适用于通过 Claude Code 使用的 **Claude Pro / Max** 订阅。

```
   ┌─ 第1页 · 概览 ──────┐        ┌─ 第2页 · 细节 ──────┐
   |     ✻ CLAUDE        |        |    ✻ DETAILS        |
   |        MAX          |        | 5H     2%    4h     |
   | SESSION       29%   |  滑屏  | WEEK   13%   5d     |
   | ▮▮▮▮▮▯▯▯▯▯▯▯▯  35m   | <-->   | SONNET 0%  OPUS --  |
   | WEEKLY        12%   | /BOOT  | EXTRA  on           |
   | ▮▮▮▯▯▯▯▯▯▯▯▯▯  5d    |        | TODAY     145M tok  |
   |   ● CLAUDE CODE     |        | ACTIVE   my-project |
   └─────────────────────┘        └─────────────────────┘
```

---

## 工作原理

用量数字藏在你的 **Claude OAuth 登录态**背后(不是 `sk-ant-…` key)。在你电脑上跑一个
极小的 **Python 代理**:它读取 Claude Code 已经存在本地的 OAuth token,调用 Anthropic
的用量接口,再把一小段 JSON 发到局域网。ESP32 只是通过 Wi-Fi 去轮询这段 JSON。

```
~/.claude/.credentials.json ──> 代理(你的电脑) ──(局域网 JSON)──> ESP32-S3 圆屏
   (Claude OAuth token)          api/oauth/usage                ClaudeOrb 固件
```

token **永远不离开你的电脑**,也**绝不会被烧进设备**。代理同时会读本地 Claude Code 日志
(`~/.claude/projects`)算出今日 token 数,以及第 2 页显示的当前活跃会话。

---

## 硬件

一块 **微雪 Waveshare ESP32-S3-Touch-LCD-1.85**(或 1.85B / 1.85C)——圆形 **360×360**
屏,**ST77916** 控制器(QSPI)+ **CST816** 触摸。约 ¥150。

> ⚠️ **板子变体的坑(屏黑就看这条)。**
> 标准版 1.85 的 LCD 复位走 **TCA9554 I²C 扩展器**(EXIO2)。
> 但有些板子(本项目用的那块,丝印 **"1.85B"**)**没有扩展器**,把 **LCD_RST 接到了
> GPIO3**。固件默认用 **GPIO3**。如果屏一直不亮,见 [适配你的板子](#适配你的板子)。

固件用到的引脚(QSPI 屏 + 触摸):

| 功能 | GPIO | 功能 | GPIO |
|---|---|---|---|
| 屏 CS | 21 | 屏 D2 | 42 |
| 屏 SCK | 40 | 屏 D3 | 41 |
| 屏 D0 | 46 | **LCD_RST** | **3**(见上注) |
| 屏 D1 | 45 | 背光 | 5 |
| 电源锁存 | 7(常驻拉高) | 触摸 I²C | SDA 11 / SCL 10 |
| 触摸 INT | 4 | BOOT 键 | 0 |

---

## 安装设置

### 1) 跑代理(你的电脑)

需要 **Python 3.8+**——**零**第三方依赖。前提是这台机器已经登录了 **Claude Code**
(`claude` → `/login`)。

```bash
python proxy/claude_limits_proxy.py
```

应当看到 `Claude 用量代理 → http://0.0.0.0:8787/usage`。浏览器打开
`http://localhost:8787/usage`,应能拿到含 `session_pct`、`week_pct`、`tok_today` 等的 JSON。

- **查本机局域网 IP**(`ipconfig` / `ip addr`)——填固件时要用。
- **放行防火墙**:首次弹窗时允许 Python 接受入站连接(它监听 `8787` 端口)。
- **直连不到 `api.anthropic.com`**(如中国大陆)?让代理走你本地的 HTTP 代理:
  `UPSTREAM_PROXY=http://127.0.0.1:7890 python proxy/claude_limits_proxy.py`(Clash 等)。

让它一直开着。开机自启见 [自启](#自启可选)。

### 2) 烧录固件(ESP32-S3)

用 **Arduino IDE / arduino-cli**,装 **esp32 core 3.x**。

1. 装库(库管理器):**GFX Library for Arduino**(作者 *moononournation*)和 **ArduinoJson**。
2. 打开 `firmware/claude_orb/claude_orb.ino`。
3. 改文件顶部:
   - `WIFI_SSID` / `WIFI_PASS`——你的 **2.4GHz** 网络(ESP32-S3 不支持 5GHz)。
   - `PROXY_URL`——`http://<你电脑的局域网IP>:8787/usage`。
4. 开发板设置:
   - 开发板:**ESP32S3 Dev Module**
   - **USB CDC On Boot: Enabled**
   - **Partition Scheme: Huge APP (3MB No OTA)**
   - **Flash Size: 16MB**
   - **PSRAM: OPI PSRAM**(当帧缓冲用,屏幕才不闪)
5. 上传。

arduino-cli 一行:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=16M,PSRAM=opi \
  -u -p <端口> firmware/claude_orb
```

> 固件画到 PSRAM 帧缓冲、只在数值变化时才整屏刷出(不闪);接了锂电时右下角显示**电量图标**。
> 电量取自板载 **BQ27220 电量计**(StateOfCharge);充电时电池变绿、带闪电,并有上涨扫描动画
> (只直接画电池那一小块,所以仍然不闪)。

开机后会连 Wi-Fi 并开始轮询代理,几秒内出数字。

### 自启(可选)

让代理重启后自动回来:

- **Windows:** 把 `proxy/ClaudeUsageProxy.vbs` 复制进启动文件夹
  (`Win+R` → `shell:startup`),它会在登录时静默启动代理。*(先改 `.vbs` 里的路径。)*
- **macOS/Linux:** 用 `systemd --user`、`launchd`、`pm2`、`nohup` 等托管。

---

## 使用

- **第 1 页 · 概览:** SESSION(5 小时窗口)和 WEEKLY(全模型)两块大表 + 实时重置倒计时。
- **第 2 页 · 细节:** 各档限额(5H / WEEK / SONNET / OPUS / EXTRA)、今日 token 总数、
  以及当前活跃的 Claude Code 会话(项目 · token · 空闲时长)。
- **翻页:** **滑动屏幕**(任意方向)或按 **BOOT** 键。

---

## 自定义

- **配色 / 主题:** 改 `.ino` 顶部的 `C_*` 常量(`C_ORANGE` 是主色,默认 Claude 橙黄 `#F4A836`)。
- **Logo:** `firmware/claude_orb/claude_logo.h` 由 `tools/gen_logo.py` 生成
  (`pip install pillow` 后跑 `python tools/gen_logo.py`)。传 `--color R,G,B` 换色。
- **刷新率:** `POLL_MS`(设备多久轮询一次代理)、代理的 `CACHE_TTL`(限额,默认 180s——
  Anthropic 对这个接口限流,别调更低)和 `DET_TTL`(本地 token 细节,默认 8s)。

### 适配你的板子

如果屏一直黑,多半是 LCD 复位/引脚不一样。固件把它们都放在 `.ino` 顶部
(`RST_PIN`、`BL_PIN`、`Arduino_ESP32QSPI(...)` 的引脚)。
**标准 1.85** 的复位走 **TCA9554 @ 0x20**(在 `gfx->begin()` 前把 EXIO2 拉高);
本项目这块 **1.85B** 是普通 GPIO(**3**)。面板需要 **微雪的 ST77916 初始化序列**
(已打包成 `st77916_ws_init.h`);GFX 库自带的通用 init 是给别的面板的,会花屏。

---

## 排错

| 现象 | 解决 |
|---|---|
| 屏黑 | 板子变体的 `RST_PIN` / 引脚不对——见上。 |
| 屏上 `NO DATA` | 代理不可达。用手机打开 `http://<电脑IP>:8787/usage`,打不开就是**防火墙**或 `PROXY_URL` 填错。 |
| 代理返回 `ok:false` / 401 | Claude 登录态/token 过期了——开一下 Claude Code(会自动刷新),或重新 `/login`。 |
| 代理连不上 Anthropic | 设 `UPSTREAM_PROXY`(见第 1 步)。 |
| Wi-Fi 连不上 | 必须是 **2.4GHz**。 |

---

## 致谢与许可

- 固件、代理、工具:**MIT**(见 `LICENSE`)。
- ST77916 面板初始化序列改编自微雪 ESP32-S3-Touch-LCD-1.85 示例代码。
- 显示驱动:[GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX)。
- "Claude" 名称与 logo 是 **Anthropic** 的商标;这里只把 logo 当作设备上的小图标用于标识。
  ClaudeOrb 是非官方的爱好者项目,与 Anthropic 无隶属、未获其背书。详见 `NOTICE`。
