# 摩突突助手 · Motutu Assistant

一台放在桌面上的 AI 编程伙伴。它把 Codex 正在干什么变成灯光和声音，让你不用一直盯着屏幕；也能直接对它说话，把下一步任务交给 Codex。

- **硬件**：LISTENAI ARCS 芯片（RISC-V 双核 AP/CP）、2.4G Wi-Fi + BLE、麦克风与扬声器、灯带、四路舵机、IMU、NFC、USB/ADB
- **软件**：设备固件 + macOS 菜单栏助手 + 本机 Python 桥接 + Codex Hooks / MCP 工具
- **许可**：Apache-2.0，可商用、可修改、可再分发

---

## 一、它能干什么

| 场景 | 你做什么 | 设备做什么 |
| --- | --- | --- |
| **编程状态灯** | 在 Codex 里正常发消息 | 拿到任务亮蓝色呼吸，等待回复青色短闪，结束时播报一句结果 |
| **审批提醒** | 需要你确认权限时不用守着屏幕 | 紫色双闪 + 语音说明审批原因，你在桌面处理即可 |
| **语音续办** | 切到办公模式，说「小聆小聆」，再说下一步 | 录音识别后排队提交给最近一轮 Codex 会话，回复「指令已排队」 |
| **语音选选项** | Codex 用 `ask_choice` 问你问题时 | 设备念出问题和选项，你直接说「选第二项」 |
| **桌面助理** | 办公模式下的日常对话 | 切回娱乐模式，保留原来的小聆对话、闹钟、音乐、音量控制 |
| **硬件被 AI 控制** | 在 Codex 里让模型调用设备工具 | 通过 MCP 调灯带、亮度、音量、闹钟、SD 卡音乐、摄像头 |
| **二次开发** | 改灯效、步态、界面、协议 | 所有源码都在本仓库，见第五节 |

> 每次开机默认是**娱乐模式**。快速四击主按键切换模式：蓝灯短闪 = 办公模式，绿灯短闪 = 娱乐模式。

---

## 二、开箱上手

### 准备

- 一台已刷好固件的摩突突设备
- Apple 芯片 Mac，macOS 13.0 及以上
- 已安装 [Codex](https://github.com/openai/codex)
- 设备提供方给你的「摩突突助手.dmg」和配对文件（`.json`）

### 第 1 步：让设备联网

用随设备提供的配网方式，让设备和 Mac 连上**同一个可互通的 Wi-Fi**。

命令行配网（Windows / macOS 通用）：

```bash
pip install -r 配网/requirements.txt
python 配网/ble_provision.py
```

设备连续按三次主按键进入配网模式，程序会扫描 BLE、列出设备、显示设备扫描到的 Wi-Fi 列表。选编号、输密码，等它真正返回「联网成功」。

也可以非交互直接配：

```bash
python 配网/ble_provision.py --device xiaoqiuquan --ssid 你的SSID --password 你的密码
```

### 第 2 步：安装桌面助手

打开 DMG，把 App 拖进「应用程序」并启动。带发布配置的安装包会自动启动本机桥接并寻找设备。

菜单里显示**设备在线**后，点「测试设备灯光与声音」——设备亮青灯，说明链路通了。

### 第 3 步：信任 Codex Hooks

在 Codex 里打开或新建一个**本地项目**，进入「设置 → Hooks」，审查并信任摩突突写入的条目。

然后在项目里发一条消息，助手显示「Codex 联动已验证」就说明接上了。

### 第 4 步：试一次语音续办

1. 菜单切到办公模式，或快速四击设备主按键
2. 等一轮 Codex 任务结束，菜单提示可以接收指令
3. 说「小聆小聆」，听到「请说下一步任务」
4. 说一句指令，比如「把刚才那个测试补上注释」
5. 听到「指令已排队」，回 Codex 看执行情况

### 第 5 步（可选）：让 Codex 反过来控制硬件

```bash
codex mcp add ls26 --url http://127.0.0.1:8766/mcp
```

然后就能在对话里说「用 LS26 的 ask_choice 问我」、「把灯调成蓝色」、「放首音乐」、「设个 30 分钟后的闹钟」。

---

## 三、卡住了先看这里

| 现象 | 检查方法 |
| --- | --- |
| 一直显示「正在寻找设备」 | 确认设备和 Mac 在同一 Wi-Fi，且路由器允许设备间互通；点「重新寻找设备」 |
| 设备在线，Codex 提示「未找到钩子」 | 先打开或新建一个**本地项目**，再看「设置 → Hooks」 |
| 已信任 Hooks，设备没有办公反馈 | 确认设备处于办公模式，再在本地项目发一条新消息 |
| 设备提示「请先在电脑上完成一轮任务」 | 先在要续办的会话里完成一轮任务，再唤醒设备 |
| 唤醒后没反应 | 确认固件开启了 `CONFIG_APP_CODEX_VOICE_INPUT`，且桥接带了 `--voice-key-file` 参数 |
| 配网一直失败 | 设备连续三击进入配网模式；Wi-Fi 必须是 2.4G，5G 不支持 |

---

## 四、仓库里有什么

```
motutu-assistant/
├── apps/arcs-mini/      CP 核业务固件：灯效、模式、语音、MCP 工具、网页控制、动作
├── apps-ui/             LVGL 界面工程（带屏版本），含 Linux 模拟器
├── arcs-sdk/            LISTENAI ARCS SDK：HAL、驱动、LVGL、网络协议栈等
├── boards/              板型定义：引脚、外设、音频回采方案
│   ├── arcs_mini_v2_1/  无屏机械形态：四路舵机 + WS2812 灯带 + IMU + NFC
│   └── arcs_mini_doll_v2/ 带屏形态
├── modules/             UI 框架、聊天、事件总线等业务模块
├── src/                 系统服务、框架、中间件
├── res/                 提示音、唤醒词、表情、资源包、分区表
├── tools/               macOS 助手、Codex 桥接、烧录工具、各种测试与调试脚本
├── 配网/                BLE 配网工具与协议文档
├── doc/flash.md         分区表、KV 存储、OTA 规则
├── build.sh / build.ps1 构建入口（Linux/macOS 与 Windows）
└── adb_download.sh/.ps1 烧录入口
```

**不在仓库里的东西**：设备配对文件、状态密钥、语音密钥、DMG 安装包、语音模型、量产工具。这些由供货方按设备逐台发放。

---

## 五、二次开发：改什么从哪里下手

| 想改的东西 | 文件位置 |
| --- | --- |
| 灯效颜色、呼吸/闪烁节奏 | `apps/arcs-mini/services/service_led.c`、`service_codex_status.c` |
| 模式切换逻辑、按键手势 | `apps/arcs-mini/services/service_button.c` |
| 四足动作、步态、舵机标定 | `apps/arcs-mini/services/motutu_motion.c`、`service_v2_actuators.c` |
| 设备网页控制面板 | `apps/arcs-mini/services/service_motutu_web.c` |
| 新增 AI 可调用的工具 | `apps/arcs-mini/mcp-tools/`（照着 `mcp_tool_led.c` 加一个即可） |
| 语音意图（闹钟、音乐、拍照） | `apps/arcs-mini/voice_intent/` |
| 提示音 | `res/arcs-mini/tone/`，保留文件名开头的编号后重新构建资源 |
| 界面、菜单、表情 | `apps-ui/`，可以在 Linux 上跑模拟器先看效果 |
| 桥接、语音识别、状态协议 | `tools/codex-status/` |
| 引脚、外设、音频回采 | `boards/arcs_mini_v2_1/`、`boards/arcs_mini_doll_v2/` |

Codex 联动部分的完整说明（桥接参数、Hooks 配置、协议帧、动态播报、审批流程）见 [tools/codex-status/README.md](tools/codex-status/README.md)。
配网协议见 [配网/README.md](配网/README.md) 和 [BLE热点列表协议.md](配网/BLE热点列表协议.md)。

---

## 六、自己编译固件

### 准备工具链

**Linux / WSL2**

```bash
source ./arcs-sdk/env.sh          # 检测 + 按需安装 + 设置环境
source ./arcs-sdk/env.sh check    # 单独体检一次
```

**macOS**

```bash
brew install listenai/tap/arcs-toolchain
```

**Windows**

```powershell
.\arcs-sdk\env.ps1
```

**其它平台（Docker）**

```bash
docker run --rm --init -w $(pwd) -v $(pwd):$(pwd) --platform linux/amd64 \
  ghcr.io/listenai/arcs-builder:latest ./build.sh -S ./apps/arcs-mini -DBOARD=arcs_mini
```

### 编译

```bash
# 当前主力板型 v2.1（软件回采）
bash ./build.sh -S apps/arcs-mini -B build-aec-software \
  -DBOARD=arcs_mini_v2_1 -DCONFIG_FILES=aec-software.conf

# 默认板型
./build.sh -S ./apps/arcs-mini -DBOARD=arcs_mini

# 带屏版本
./build.sh -S ./apps/arcs-mini -B build-doll_v2 -DBOARD=arcs_mini_doll_v2
```

要启用 Codex 联动，另建一个覆盖配置（放在仓库外，别提交）：

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

## 七、烧录与调试

### 首选用 ADB

```bash
bash adb_download.sh -S res/arcs-mini app     # 只烧 CP 应用（日常改代码）
bash adb_download.sh -S res/arcs-mini         # 应用 + 资源
bash adb_download.sh -S res/arcs-mini boot    # 连 boot 一起烧
```

```powershell
.\adb_download.ps1 -S res\arcs-mini app
```

### 救砖用串口

只在进不了 ADB（boot 损坏、首次上电、刷错）时用：

```bash
cskburn -C arcs -b 3000000 -s /dev/ttyACM0 --verify-all 0x600000 build/arcs-mini.bin
```

macOS 串口一般是 `/dev/cu.usbmodem*`，Windows 是 `COM3` 之类。多个分区可以一次写：

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

完整的分区表、地址偏移、KV 键名、OTA 覆盖范围见 [doc/flash.md](doc/flash.md)。记住三条：

- 普通业务改动只烧 `app@0x600000`
- 资源变化才烧对应资源分区（`tone` / `wake_word` / `emoji` / `respak`）
- `kv`、`flash_fs`、`wifi_nvs` 不要手工写，`kv clean` 等于恢复出厂

---

## 八、协议速查

### Codex 状态（WebSocket）

设备连接 `/status/<token>`，桥接发文本帧：

- `1 S <state>`：连接时同步当前状态
- `1 E <state>`：一次状态变化
- 状态值：`idle` / `working` / `approval` / `done`

带动态播报的板子连 `/status/<token>/audio`，可收到 `1 E done <clip-id>`，再从同一主机的 `/audio/<clip-id>.mp3` 播放。

设备自动发现走 UDP `8767`，状态端口默认 `8765`，Hooks 回环端口 `8766`。

### BLE 配网

- GATT 服务 `0xE402`，写入/通知特征 `0xE403`
- 命令：`A001 START`、`A002 SSID`、`A003 PASSWORD`、`A010 DONE`
- 只读：`A014 DEVICE_INFO`（ChipID / SN / 固件版本）
- 扫描：`A015 WIFI_SCAN`，分页返回 `wifi_scan` JSON

---

## 九、常见问题

**Q：固件和桌面助手必须同一个版本吗？**
不必须，但状态协议和密钥要一致。跨版本升级时先升助手，再升固件。

**Q：能在 Windows 上跑桥接吗？**
协议和 Python 桥接是通的，Hooks 路径、启动方式和防火墙还没在 Windows 上验收。

**Q：语音识别一定要联网吗？**
不需要。语音续办用的是本机 SenseVoiceSmall，录音只在本次唤醒后采集，识别完就删临时文件。但 Wi-Fi 传输仍是明文，只适合可信局域网。

**Q：买家拿到配对文件后要改什么？**
不用改代码，导入配对文件即可。配对文件里的 `status_token` 对应固件状态密钥，`voice_key` 对应语音校验密钥，每台设备不同，由供货方单独发放。

**Q：可以商用吗？**
可以。代码是 Apache-2.0，允许商用、修改、再分发，附带专利授权。第三方组件许可见各自目录下的 LICENSE。

---

## 十、目录外的东西

| 文件 | 谁提供 | 说明 |
| --- | --- | --- |
| 摩突突助手.dmg | 供货方 | 带运行时的安装包，含桥接、Python、SenseVoiceSmall |
| 配对文件 `.json` | 供货方 | 每台设备独立的状态密钥与语音密钥 |
| 设备固件 | 供货方 / 自行编译 | 量产固件由供货方烧录，也可用本仓库源码自行构建 |
| SenseVoice / Whisper 模型 | 自行准备或随包 | 模型文件大于仓库限制，不进版本库 |

---

有问题先看 [tools/codex-status/README.md](tools/codex-status/README.md) 和 [doc/flash.md](doc/flash.md)，这两份记录了大部分坑。
