# 摩突突 · AI 电子狗（Motutu）

一只会走路、会摇头、会发光、会说话的桌面 AI 电子狗。

它内置四足舵机、WS2812 灯带、麦克风与扬声器、IMU、NFC，能听懂唤醒词跟你对话，也能和另一只狗碰一碰互相认身份。接上 Codex 之后，它还能变成你写代码时的桌面搭子——任务跑到哪一步、要不要你审批，它用灯光和语音告诉你；你也可以直接对它说下一步，让 Codex 接着干。

> **本仓库定位**：目前的源码以 **Codex 对接** 为主，同时包含设备固件底座、板级配置、烧录与调试工具链。
> 跳跃、养成系统、双人互动、外接传感器这些玩法都建立在这套底座之上，仓库里已经有可复用的积木（动作序列、灯语、语音、NFC、网页 API、MCP 工具），后续会持续开放更多好玩的外接能力。

---

## 它都能玩什么

| 玩法 | 现在就能用 | 你可以自己扩展 |
| --- | --- | --- |
| **四足动作** | 站起、趴下、行走、后退、原地转圈、招手、作揖、翻身、摇爪 | 用 JSON 动作序列编舞步：抬腿、蹲起、跳跃、连续连招 |
| **自己调动作** | 浏览器打开设备网页控制台，实时下发动作序列 | 接上动作工作室批量调参、录一套连招 |
| **灯语** | 状态灯带：待机、思考、审批、完成各有颜色和节奏 | 1–64 颗 WS2812 任意配色，做呼吸、流水、心跳 |
| **会说话** | 唤醒词「小聆小聆」，语音对话、任务播报、闹钟、放歌 | 换唤醒词、换音色、加自己的语音意图 |
| **两只狗互动** | 两台设备 NFC 碰一碰，互相识别身份（狗子A ↔ 狗子B） | 做握手、贴贴、组队、抢地盘等双人玩法 |
| **养成玩法** | 已有音量、亮度、闹钟、音乐、电量、情绪动作等基础积木 | 加喂食、摸摸、亲密度、心情值、成长阶段，做成桌面宠物 |
| **AI 编程搭子** | Codex 状态灯、审批提醒、语音续办、语音选选项 | 让 AI 通过 MCP 直接控制狗子的灯、动作、音乐、摄像头 |
| **外接扩展** | 四路舵机、灯带、I2C（IMU / NFC）、触摸按键、USB/ADB | 加传感器、加屏幕（带屏板型）、接机械臂或小车底盘 |

开箱默认是**娱乐模式**（小聆对话）。快速四击主按键切换：蓝灯短闪 = 办公模式，绿灯短闪 = 娱乐模式。

---

## 五分钟上手（Codex 联动）

### 准备

- 一台已刷好固件的摩突突
- Apple 芯片 Mac，macOS 13.0 及以上
- 已安装 [Codex](https://github.com/openai/codex)
- 供货方提供的「摩突突助手.dmg」和配对文件（`.json`）

### 1. 让狗子联网

设备和 Mac 连同一个可互通的 Wi-Fi（只支持 2.4G）。

```bash
pip install -r 配网/requirements.txt
python 配网/ble_provision.py
```

设备连续按三次主按键进入配网模式，程序会列出设备扫描到的 Wi-Fi，选编号输密码，等它返回「联网成功」。

也可以直接命令行配：

```bash
python 配网/ble_provision.py --device xiaoqiuquan --ssid 你的SSID --password 你的密码
```

### 2. 装桌面助手

打开 DMG，拖进「应用程序」启动。带发布配置的安装包会自动拉起本机桥接并寻找设备。

菜单显示**设备在线**后点「测试设备灯光与声音」，狗子亮青灯就说明通了。

### 3. 信任 Codex Hooks

在 Codex 里打开或新建一个**本地项目**，进入「设置 → Hooks」审查并信任摩突突的条目。发一条消息，助手显示「Codex 联动已验证」即完成。

### 4. 试一次语音续办

1. 切到办公模式（菜单，或快速四击主按键）
2. 等一轮 Codex 任务结束
3. 说「小聆小聆」，听到「请说下一步任务」
4. 说一句指令，比如「把刚才那个测试补上注释」
5. 听到「指令已排队」，回 Codex 看执行情况

### 5. 让 AI 反过来指挥狗子

```bash
codex mcp add ls26 --url http://127.0.0.1:8766/mcp
```

之后可以直接说：「用 LS26 的 ask_choice 问我」「把灯调成蓝色」「放首歌」「设个 30 分钟后的闹钟」。

---

## 和 Codex 配合的四个场景

| 场景 | 你做什么 | 狗子做什么 |
| --- | --- | --- |
| **编程状态灯** | 正常在 Codex 里发消息 | 接到任务蓝色呼吸，等回复青色短闪，结束时播报一句结果 |
| **审批提醒** | 需要授权时不用守着屏幕 | 紫色双闪 + 语音说明审批原因，你在桌面处理 |
| **语音续办** | 「小聆小聆」+ 一句指令 | 本机识别后投递给最近一轮会话，回「指令已排队」 |
| **语音选选项** | Codex 用 `ask_choice` 提问 | 念出问题和选项，你说「选第二项」 |

---

## 自己动手玩

### 直接命令控制（ADB shell）

```bash
adb shell dog                 # 不带参数，打印全部可用命令
adb shell dog stand           # 站起来
adb shell dog walk            # 走两步
adb shell dog spin_left       # 原地转圈
adb shell dog shake_left      # 抬左爪
adb shell dog wave_left       # 招招手
adb shell dog serve_left      # 作揖
adb shell dog flip            # 翻身
adb shell dog seq '<json>'    # 执行一段自定义动作序列
```

`dog` 的完整能力还包括 `home` / `down` / `gait`（步态显示与逐相位调试）/ `pose` / `joint` / `pulse` / `calib`（舵机标定）。想加跳跃这类动作，用 `seq` 下发关节轨迹，或者在 `motutu_motion.c` 里照着 `motutu_flip()` 写一个即可。

### 网页控制台

设备联网后，浏览器打开：

```
http://设备IP:8080
```

可以看到狗子状态、四足关节角度，并实时下发动作序列。`POST /api/dog/sequence` 接受一段 JSON 描述动作，立即执行——这也是自己编舞步、加跳跃动作的入口。

### 两只狗碰一碰

```bash
python3 tools/nfc_peer_dashboard.py
```

两块板靠近时自动交换身份，页面弹出「已识别身份：狗子A ↔ 狗子B」。

### 想做养成系统？

底座已经有全部积木，可以直接往上搭：

| 需要的东西 | 现成的能力 |
| --- | --- |
| 记忆状态（饥饿、心情、亲密度） | EasyFlash KV，键值持久化，见 [doc/flash.md](doc/flash.md) |
| 表达情绪 | 灯带颜色 + 四足动作 + 提示音，三件套组合 |
| 让 AI 参与养成 | `apps/arcs-mini/mcp-tools/` 里照着加一个 `mcp_tool_feed.c`，模型就能调用 |
| 交互入口 | 触摸按键、语音意图、网页控制台、Codex MCP，四条路都通 |

---

## 卡住了先看这里

| 现象 | 检查方法 |
| --- | --- |
| 一直显示「正在寻找设备」 | 确认设备和 Mac 在同一 Wi-Fi，路由器允许设备互通；点「重新寻找设备」 |
| 设备在线，Codex 提示「未找到钩子」 | 先打开或新建一个**本地项目**，再看「设置 → Hooks」 |
| 已信任 Hooks，设备没有办公反馈 | 确认狗子在办公模式，再在本地项目发一条新消息 |
| 提示「请先在电脑上完成一轮任务」 | 先在要续办的会话里完成一轮任务，再唤醒 |
| 唤醒后没反应 | 确认固件开了 `CONFIG_APP_CODEX_VOICE_INPUT`，桥接带了 `--voice-key-file` |
| 配网一直失败 | 连续三击进配网模式；Wi-Fi 必须是 2.4G |

---

## 仓库里有什么

```
motutu-assistant/
├── apps/arcs-mini/      CP 核业务固件：动作、灯效、模式、语音、MCP 工具、网页控制
├── apps-ui/             LVGL 界面工程（带屏板型），含 Linux 模拟器
├── arcs-sdk/            LISTENAI ARCS SDK：HAL、驱动、LVGL、网络协议栈
├── boards/              板型定义：引脚、外设、音频回采方案
│   ├── arcs_mini_v2_1/    四足形态：四路舵机 + WS2812 + IMU + NFC，无屏
│   └── arcs_mini_doll_v2/ 带屏形态
├── modules/             UI 框架、聊天、事件总线
├── src/                 系统服务、框架、中间件
├── res/                 提示音、唤醒词、表情、资源包、分区表
├── tools/               macOS 助手、Codex 桥接、烧录工具、NFC/动作调试脚本
├── 配网/                BLE 配网工具与协议文档
├── doc/flash.md         分区表、KV 存储、OTA 规则
├── build.sh / build.ps1 构建入口
└── adb_download.sh/.ps1 烧录入口
```

**不在仓库里的**：设备配对文件、状态密钥、语音密钥、DMG 安装包、语音模型、量产工具。这些由供货方按设备逐台发放。

---

## 二次开发：改什么从哪里下手

| 想改的东西 | 文件位置 |
| --- | --- |
| 四足动作、步态、舵机标定、跳跃 | `apps/arcs-mini/services/motutu_motion.c`、`service_v2_actuators.c` |
| 灯效颜色、呼吸/闪烁节奏 | `apps/arcs-mini/services/service_led.c`、`service_codex_status.c` |
| 模式切换、按键手势 | `apps/arcs-mini/services/service_button.c` |
| 设备网页控制台、动作序列接口 | `apps/arcs-mini/services/service_motutu_web.c` |
| 新增 AI 可调用的工具 | `apps/arcs-mini/mcp-tools/`（照着 `mcp_tool_led.c` 加） |
| 语音意图（闹钟、音乐、拍照） | `apps/arcs-mini/voice_intent/` |
| 提示音 | `res/arcs-mini/tone/`，保留文件名开头的编号后重新构建资源 |
| 界面、菜单、表情 | `apps-ui/`，可先在 Linux 跑模拟器 |
| 桥接、语音识别、状态协议 | `tools/codex-status/` |
| 引脚、外设、音频回采 | `boards/arcs_mini_v2_1/`、`boards/arcs_mini_doll_v2/` |

Codex 联动完整说明（桥接参数、Hooks、协议帧、动态播报、审批流程）见 [tools/codex-status/README.md](tools/codex-status/README.md)。
配网协议见 [配网/README.md](配网/README.md) 与 [BLE热点列表协议.md](配网/BLE热点列表协议.md)。

---

## 自己编译固件

### 工具链

**Linux / WSL2**

```bash
source ./arcs-sdk/env.sh          # 检测 + 按需安装 + 设置环境
source ./arcs-sdk/env.sh check    # 单独体检
```

**macOS**

```bash
brew install listenai/tap/arcs-toolchain
```

**Windows**

```powershell
.\arcs-sdk\env.ps1
```

**Docker**

```bash
docker run --rm --init -w $(pwd) -v $(pwd):$(pwd) --platform linux/amd64 \
  ghcr.io/listenai/arcs-builder:latest ./build.sh -S ./apps/arcs-mini -DBOARD=arcs_mini
```

### 编译

```bash
# 主力板型 v2.1（软件回采）
bash ./build.sh -S apps/arcs-mini -B build-aec-software \
  -DBOARD=arcs_mini_v2_1 -DCONFIG_FILES=aec-software.conf

# 默认板型
./build.sh -S ./apps/arcs-mini -DBOARD=arcs_mini

# 带屏版本
./build.sh -S ./apps/arcs-mini -B build-doll_v2 -DBOARD=arcs_mini_doll_v2
```

启用 Codex 联动需要覆盖配置（放仓库外，别提交）：

```ini
CONFIG_APP_CODEX_STATUS=y
CONFIG_APP_CODEX_STATUS_HOST="你的 Mac 局域网 IP"
CONFIG_APP_CODEX_STATUS_PORT="8765"
CONFIG_APP_CODEX_STATUS_TOKEN="桥接启动时生成的 token"
```

```bash
bash ./build.sh -S apps/arcs-mini -B build-aec-software \
  -DBOARD=arcs_mini_v2_1 '-DCONFIG_FILES=aec-software.conf;/tmp/codex-status.conf'
```

### 编译桌面助手

```bash
bash tools/codex-status/macos/build.sh /tmp/motutu-assistant
open "/tmp/motutu-assistant/摩突突助手.app"
```

---

## 烧录与调试

### 首选 ADB

```bash
bash adb_download.sh -S res/arcs-mini app     # 只烧 CP 应用（日常改代码）
bash adb_download.sh -S res/arcs-mini         # 应用 + 资源
bash adb_download.sh -S res/arcs-mini boot    # 连 boot 一起烧
```

```powershell
.\adb_download.ps1 -S res\arcs-mini app
```

### 救砖用串口

```bash
cskburn -C arcs -b 3000000 -s /dev/ttyACM0 --verify-all 0x600000 build/arcs-mini.bin
```

macOS 串口一般是 `/dev/cu.usbmodem*`，Windows 是 `COM3` 之类。多分区可一次写：

```bash
cskburn -C arcs -b 3000000 -s /dev/ttyACM0 --verify-all \
  0x40000 ./res/arcs-mini/ap.bin 0x100000 ./res/arcs-mini/tone.bin
```

### 抓日志

```bash
mkdir -p ./tmp && adb shell reboot && adb wait-for-device && timeout 10 adb shell 2>&1 > ./tmp/run-log.txt
```

没有 ADB 时走串口，波特率固定 `921600`：

```bash
timeout 10 picocom -b 921600 --lower-dtr --lower-rts /dev/ttyACM0 > ./tmp/run-log.txt
```

崩溃地址还原：

```bash
riscv64-unknown-elf-addr2line -e build/arcs-mini -a 0xAAAAAAAA
```

### 分区与 OTA

完整分区表、地址偏移、KV 键名、OTA 覆盖范围见 [doc/flash.md](doc/flash.md)。记住三条：

- 普通业务改动只烧 `app@0x600000`
- 资源变化才烧对应资源分区（`tone` / `wake_word` / `emoji` / `respak`）
- `kv`、`flash_fs`、`wifi_nvs` 不要手工写，`kv clean` 等于恢复出厂

---

## 协议速查

### Codex 状态（WebSocket）

设备连接 `/status/<token>`，桥接发文本帧：

- `1 S <state>`：连接时同步当前状态
- `1 E <state>`：一次状态变化
- 状态值：`idle` / `working` / `approval` / `done`

带动态播报的板子连 `/status/<token>/audio`，收到 `1 E done <clip-id>` 后从同一主机 `/audio/<clip-id>.mp3` 播放。

自动发现走 UDP `8767`，状态端口默认 `8765`，Hooks 回环端口 `8766`，设备网页控制台 `8080`。

### BLE 配网

- GATT 服务 `0xE402`，写入/通知特征 `0xE403`
- 命令：`A001 START`、`A002 SSID`、`A003 PASSWORD`、`A010 DONE`
- 只读：`A014 DEVICE_INFO`（ChipID / SN / 固件版本）
- 扫描：`A015 WIFI_SCAN`，分页返回 `wifi_scan` JSON

---

## 常见问题

**Q：能自己加动作吗？**
能。走网页控制台的 `/api/dog/sequence` 下发 JSON 动作序列，或者在 `motutu_motion.c` 里加固定的动作函数。跳跃就是「四条腿同时蓄力 → 伸展」的序列。

**Q：养成系统要自己写吗？**
需要，但底座给全了：KV 存心情和饥饿值、灯带和动作做反馈、MCP 工具让 AI 参与、触摸和语音做入口。

**Q：固件和桌面助手必须同版本吗？**
不必须，但状态协议和密钥要一致。跨版本升级先升助手，再升固件。

**Q：能在 Windows 上跑桥接吗？**
协议和 Python 桥接是通的，Hooks 路径、启动方式和防火墙还没在 Windows 验收。

**Q：语音识别一定要联网吗？**
不用。语音续办用本机 SenseVoiceSmall，录音只在本次唤醒后采集，识别完删临时文件。Wi-Fi 传输仍是明文，只适合可信局域网。

**Q：可以商用吗？**
可以。代码 Apache-2.0，允许商用、修改、再分发，附带专利授权。第三方组件许可见各自目录下的 LICENSE。

---

## 目录外的东西

| 文件 | 谁提供 | 说明 |
| --- | --- | --- |
| 摩突突助手.dmg | 供货方 | 带运行时的安装包，含桥接、Python、SenseVoiceSmall |
| 配对文件 `.json` | 供货方 | 每台设备独立的状态密钥与语音密钥 |
| 设备固件 | 供货方 / 自行编译 | 量产固件由供货方烧录，也可用本仓库源码构建 |
| SenseVoice / Whisper 模型 | 自行准备或随包 | 模型超出仓库限制，不进版本库 |

---

遇到问题先看 [tools/codex-status/README.md](tools/codex-status/README.md) 和 [doc/flash.md](doc/flash.md)，这两份记录了大部分坑。
