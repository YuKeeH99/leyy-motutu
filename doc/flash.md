# ARCS-MINI Flash 分区与持久化数据说明

本文以 `res/arcs-mini/partition_table.json`、`apps/arcs-mini/prj.conf` 和
`build-half-talking/.config` 为准，记录 `arcs_mini_v2_1` 当前镜像的下载地址、
分区边界、OTA 覆盖范围以及已知持久化 KV。本文只记录键名和用途，不保存设备的
Wi-Fi 密码、License、SID、Token 等实际敏感值。

## 1. 地址规则

- Flash 映射基址：`0x30000000`。
- 烧录工具和分区表使用相对 Flash 偏移，例如 app 使用 `0x600000`。
- CPU 映射地址 = `0x30000000 + 相对偏移`，因此 app 映射地址为 `0x30600000`。
- 当前分区表规划前 16 MiB，即相对偏移 `0x000000..0xFFFFFF`。
- 驱动配置的芯片窗口是 32 MiB；后 16 MiB 未被当前分区表分配，不得自行写入。
- 擦除最小单位和本项目分区对齐单位均为 4 KiB。

## 2. 完整分区表

表中的“容量”是分区最大容量，不是当前文件大小。

| 分区 | 下载偏移 | CPU 映射地址 | 结束偏移 | 容量 | 用途 | 打包/OTA |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| `boot` | `0x000000` | `0x30000000` | `0x03DFFF` | 248 KiB | Bootloader | LPK；仅显式选择 boot 时烧录 |
| `boot_control` | `0x03E000` | `0x3003E000` | `0x03EFFF` | 4 KiB | 启动模式、OTA 请求和失败状态 | 运行时读写，不进镜像 |
| 保留间隙 | `0x03F000` | `0x3003F000` | `0x03FFFF` | 4 KiB | 分区对齐保留 | 禁止写入 |
| `ap` | `0x040000` | `0x30040000` | `0x0FFFFF` | 768 KiB | AP 核算法固件 | LPK、combined HEX、应用 OTA |
| `tone` | `0x100000` | `0x30100000` | `0x1FFFFF` | 1 MiB | 提示音 ROMFS | LPK、combined HEX、资源 OTA |
| `wake_word` | `0x200000` | `0x30200000` | `0x37FFFF` | 1.5 MiB | 唤醒词 ROMFS | LPK、combined HEX、资源 OTA |
| `emoji` | `0x380000` | `0x30380000` | `0x43FFFF` | 768 KiB | 表情资源 ROMFS | LPK、combined HEX、资源 OTA |
| `respak` | `0x440000` | `0x30440000` | `0x5FFFFF` | 1.75 MiB | 通用资源包 | LPK、combined HEX；当前无独立资源 OTA |
| `app` | `0x600000` | `0x30600000` | `0x9FFFFF` | 4 MiB | CP 核业务应用 | LPK、combined HEX、应用 OTA |
| `ota` | `0xA00000` | `0x30A00000` | `0xEFFFFF` | 5 MiB | `.ota.txz` 下载暂存区 | Boot 校验后把包内 AP/app 写入最终分区 |
| `kv` | `0xF00000` | `0x30F00000` | `0xF07FFF` | 32 KiB | EasyFlash/LISA KV | 不参与普通烧录和 OTA |
| `flash_fs` | `0xF08000` | `0x30F08000` | `0xFEFFFF` | 928 KiB | FAT 文件系统，卷名 `NAND` | 运行时文件，不参与 OTA |
| `wifi_nvs` | `0xFF0000` | `0x30FF0000` | `0xFF3FFF` | 16 KiB | Wi-Fi manager 预留 NVS | 不参与普通烧录和 OTA |
| 尾部保留 | `0xFF4000` | `0x30FF4000` | `0xFFFFFF` | 48 KiB | 后续扩展及安全间隔 | 禁止写入 |

必须始终满足：

```text
kv.end       = 0xF08000
flash_fs.begin = 0xF08000
flash_fs.end = 0xFF0000
wifi_nvs.begin = 0xFF0000
wifi_nvs.end = 0xFF4000
```

`apps/arcs-mini/main.c` 已加入编译期断言，EasyFlash、Flash FAT 和 Wi-Fi NVS 任意
重叠都会导致编译失败。旧布局曾让 FAT 和 EasyFlash 同时从 `0xF00000` 开始，挂载
文件系统会格式化 KV 扇区，表现为 SN、云端配置和网络配置重启后消失。

## 3. 当前镜像文件

下表是 2.6.1 构建使用的资源文件及实际 app 大小；上传或烧录前仍应以实际产物为准，
并检查文件大小不超过上表容量。

| 分区 | 文件 | 典型大小 |
| --- | --- | ---: |
| `boot` | `res/arcs-mini/boot.bin` | 221,992 bytes |
| `ap` | `res/arcs-mini/ap.bin` | 392,988 bytes |
| `tone` | `res/arcs-mini/tone.bin` | 473,088 bytes |
| `wake_word` | `res/arcs-mini/wake_word.bin` | 1,432,576 bytes |
| `emoji` | `res/arcs-mini/emoji.bin` | 491,520 bytes |
| `respak` | `res/arcs-mini/respak.bin` | 983,040 bytes |
| `app` | `build/arcs-mini.bin` | 2,693,504 bytes |

后台“应用 OTA”上传的是构建目录中的 `.ota.txz`，不是裸 `arcs-mini.bin`。当前包由
分区表中带 `ota` 标签的 `ap + app` 组成，先完整下载到 `ota@0xA00000`，再由 Boot
应用到 `ap@0x040000` 和 `app@0x600000`。KV、Flash FAT、Wi-Fi NVS 和资源分区不会
被应用 OTA 包覆盖。

## 4. EasyFlash KV 区域

### 4.1 存储规则

- EasyFlash 相对偏移：`0xF00000`。
- CPU 映射地址：`0x30F00000`。
- 环境区大小：32 KiB，范围 `0x30F00000..0x30F07FFF`。
- Key 最大长度：32 字节；最小擦除单位：4 KiB。
- EasyFlash 是日志式存储。Key 没有固定的物理字节地址，更新和垃圾回收后位置会变化；
  不允许按绝对地址直接改某一个 KV。

### 4.2 设备身份和云端配置

| KV key | 类型 | 含义 |
| --- | --- | --- |
| `user.lingxin.app_id` | string | 量产工具整包写入的聆思 App/租户 ID |
| `user.lingxin.license` | string | 量产工具整包写入的聆思 License，敏感信息 |
| `user.lingxin.sn` | string | 量产设备 SN；应用 OTA 的 `x-deviceid` 使用此值 |
| `user.lingxin.app_code` | string | 量产工具整包写入的 Agent/App Code |
| `user.lingxin.device_code` | string | 量产工具整包写入的设备型号代码 |
| `user.lingxin.provisioned` | int | 五项写入并回读一致后最后置 `1` 的事务完成标记 |
| `lingxin.app_id` 等五项 | string | 旧版量产工具兼容区；仅在无完成标记且五项完整时整包迁移 |
| `lingxin.chat.session_id` | int | 对话 session 自增编号 |
| `user.pid` | string | 资源平台 Product ID |
| `user.sid` | string | 云端 SID，敏感信息 |
| `user.device.id` | string | 用户覆盖的设备 ID |
| `user.appid` | string | 兼容云端 App ID |
| `user.appkey` | string | 兼容云端 App Key，敏感信息 |
| `user.token` | string | 访问 Token，敏感信息 |
| `user.device_mode` | int | 设备环境模式，如生产/预发/集成环境 |

量产信息必须使用 `dist/LifelikeCoreMassTool/LifelikeCoreMassTool.exe` 写入。工具调用
`lingxin config-set <app_id> <license> <sn> <app_code> <device_code>`；固件先清除完成标记，
整组写入、整组回读校验，最后才把 `user.lingxin.provisioned` 置为 `1`。固件不会再用
Kconfig 默认值补字段，也不会从 HTTP 接口下载身份，因而不会把不同来源的字段混成一组。
未完成量产写入时云端连接被禁止，唤醒后播放“请写入阿里灵芯参数”。`lingxin config`
会隐藏 License 明文。

### 4.3 网络与 Wi-Fi 凭据

当前 Wi-Fi manager 的热点列表通过 LISA KV 存在 EasyFlash 区，而不是把 SSID/密码
当成固定结构直接写到 `wifi_nvs` 地址。

| KV key/格式 | 类型 | 含义 |
| --- | --- | --- |
| `wifi-kv-count` | int | 新格式已保存热点数量 |
| `wifi-kv-item-N-version` | int | 第 N 条记录格式版本 |
| `wifi-kv-item-N-enable` | bool | 该热点是否启用 |
| `wifi-kv-item-N-ssid` | string | SSID |
| `wifi-kv-item-N-pwd` | string | Wi-Fi 密码，敏感信息 |
| `wifi-kv-item-N-bssid` | string | AP BSSID |
| `wifi-kv-item-N-channel` | int | Wi-Fi 信道 |
| `wifi-kv-item-N-rssi` | int | 最近记录的信号强度 |
| `wifi-kv-item-N-encryption` | int | 加密方式枚举 |
| `wifi-kv-item-N-pmk_valid` | bool | PMK 是否有效 |
| `wifi-kv-item-N-pmk` | blob | PMK，敏感信息 |
| `wifi-info-count` | int | 旧格式热点数量，仅用于迁移 |
| `wifi-list` | blob | 旧格式热点列表，仅用于迁移 |
| `user.network_mode` | int | Wi-Fi/蜂窝等网络承载选择 |

`N` 从 0 开始。连接成功事件 `WIFI_MGR_STA_CONNECTED` 到达后才保存新热点。查看
`v2hw netcfg status` 时，`saved_ap=yes` 表示凭据存在；`NO_AP` 只表示本轮没有扫描到
目标 SSID，不能据此判断凭据已清除。

### 4.4 音频、交互和界面配置

| KV key | 类型 | 含义 |
| --- | --- | --- |
| `user.volume` | int | 主音量，0..100 |
| `user.min_volume` | int | 最低非静音音量 |
| `user.brightness` | int | 屏幕/灯光亮度 |
| `user.mic_gain_db` | int | 麦克风增益 |
| `user.aec_gain_db` | int | AEC 参考增益 |
| `user.spk_gain_db` | int | 扬声器增益 |
| `user.wakeup_mode` | int | 唤醒/工作模式 |
| `user.int_mode` | int | 对话交互模式 |
| `user.full_duplex_timeout_ms` | int | 全双工兼容模式超时 |
| `user.idle_exit_timeout_ms` | int | 对话空闲退出超时 |
| `user.standby_sleep_delay_ms` | int | 待机休眠延迟 |
| `user.tts_text_ms_per_char` | int | TTS 文本显示/估时参数 |
| `user.locale` | string | 语言区域设置 |
| `user.camera.hmirror` | int | 摄像头水平镜像开关 |
| `user.camera.flip` | int | 摄像头垂直翻转开关 |
| `user.v2.rgb.calibration` | blob | V2 RGB 标定记录 |

未设置的项目使用对应 Kconfig/业务默认值；具体合法范围由读取该键的业务模块校验。

### 4.5 OTA 和资源状态

| KV key | 类型 | 含义 |
| --- | --- | --- |
| `sys.ota.pending_pkg` | string | 正在等待 Boot 应用的 `package_id` |
| `sys.ota.pending_ver` | string | 正在等待应用的目标版本 |
| `sys.ota.failed_pkg` | string | 上次失败包黑名单，防止循环升级 |
| `sys.wakeword` | string | 当前资源配置对应的唤醒词文本 |
| `user.disable_app_update` | int | 0=允许应用 OTA，非 0=禁止 |
| `user.disable_wakeword_update` | int | 0=允许唤醒词资源 OTA，非 0=禁止 |
| `user.disable_tone_update` | int | 0=允许提示音资源 OTA，非 0=禁止 |
| `user.disable_emoji_update` | int | 0=允许表情资源 OTA，非 0=禁止 |

`boot_control@0x03E000` 与上述 OTA KV 不同：前者供 Boot 和应用交换升级请求、启动模式
及最终结果，后者供业务层记录包 ID、目标版本和失败包。两者都不应手工擦写。

### 4.6 其它业务 KV

| KV key/格式 | 类型 | 含义 |
| --- | --- | --- |
| `user.alarm_list` | blob | 闹钟 cloud_id 索引列表 |
| `<alarm_cloud_id>` | blob | 纯数字 key，对应单个闹钟结构 |
| `user.alarm_ring_temp_volume` | int | 闹钟播放临时音量 |
| `bat_ui_pct` | int | 最近保存的电池 UI 百分比 |
| `sd.card.cid` | string | SD 卡 CID |
| `sd.card.stamp` | string | SD 卡内容同步标记 |
| `sd.card.present` | string | 最近检测到的 SD 卡标识 |

## 5. `wifi_nvs` 与 Flash FAT

- `flash_fs@0xF08000` 是 FAT 卷 `NAND`，只能通过文件系统接口访问，不可当 KV 使用。
- `wifi_nvs@0xFF0000` 按 Wi-Fi manager Kconfig 保留 16 KiB。当前应用层热点列表使用上节
  的 LISA KV 键；该 NVS 区仍按 SDK 内部保留区管理，不要用 `kv` 命令或裸地址写入。
- 两个区域均不属于应用 OTA。恢复出厂或全片擦除可能清空它们。

## 6. 安全的读取和验证命令

进入 ADB shell 或 921600 波特率串口后执行：

```text
version
lingxin config
v2hw netcfg status
wifi status
ota status
```

通用 KV 命令：

```text
kv get int <key>
kv get bool <key>
kv get string <key>
kv get blob <key>
kv set int <key> <value>
kv set bool <key> <0|1>
kv set string <key> <value>
kv del <key>
```

注意：

- `kv show` 可能打印 Wi-Fi 密码、Token、License 等内容，不要把完整输出发到公共日志。
- 不要执行 `kv clean`，除非明确要恢复出厂；它会删除设备身份、Wi-Fi 和用户设置。
- 不要用烧录工具直接写 `0xF00000`、`0xF08000`、`0xFF0000`。
- 普通业务修改只烧 `app@0x600000`；资源变化才烧对应资源分区；Boot 变化才显式烧 boot。

## 7. 版本与后台上传

版本源文件：`apps/arcs-mini/version`。数字版本计算规则：

```text
version_number = major * 1000000 + minor * 1000 + build
```

本次版本从 `2.6.0` 增加到 `2.6.1`，对应 `version_number=2006001`。构建完成后后台上传
文件应为：

```text
build-half-talking/arcs-mini_v2_1-v2.6.1.ota.txz
```

上传后台前应同时核对生成的 `ota-info.txt` 中版本、AP/app 地址、文件大小和摘要。

本次已生成并核对：

| 项目 | 值 |
| --- | --- |
| OTA 文件大小 | 1,646,556 bytes |
| OTA MD5 | `4701F1E985D24E0CBD0C779B6FB39EB4` |
| OTA SHA-256 | `F6F171E7D2F8EC9CC42F2FAA0C3E3A4E7E65A287DAEDF326103B9E995A1379CD` |
| app MD5 | `94B93C2DF2B7F011EEBEC149265F7914` |
| app SHA-256 | `3D844AA13781BCB61A23FCBD94FC626FCA03D1990EBE48A71DE91CB16B39809C` |

包内 `config.json` 已确认最终写入地址为 `ap@0x30040000` 和
`app@0x30600000`，没有 KV、FAT、Wi-Fi NVS 或资源分区条目。

## 8. ADB recovery 可靠性修复（2026-09-09）

`arcs_mini_v2_1` 的 boot 必须同时启用 `CONFIG_BOOT_ADB_SYNC=y`，只有
`CONFIG_BOOT_ADB=y` 时虽然可能枚举为 recovery 设备，但不具备镜像 `push` 能力。
当前 boot 构建位置固定为 `build/boot/boot.bin`，并同步到分区表引用的
`res/arcs-mini/boot.bin`；大小为 173,980 bytes，未超过 boot 分区上限。

应用和 ADB 的 `reboot recovery` 入口统一写入 `SOFT` recovery 原因后软复位，下载脚本
优先执行产品 shell 的 `recovery` 命令。Windows 下设备只有序列号、没有
`transport_id`/`usb` 标识时，只允许在“恰好一台正常设备、且没有初始 BOOT 设备”的
前提下进入单设备回退匹配；多设备或多个 BOOT 候选会拒绝烧录，避免写错设备。

软请求进入 recovery 后，如果 30 秒内没有收到真正的 ADB `CNXN` 协议握手，boot 会
清除临时 recovery 请求并恢复正常启动，避免只能拔电恢复。手动/硬件 recovery、应用
镜像损坏和看门狗恢复不使用该超时逃生，以保留救砖通道。

旧 boot 不含上述修复，因此需要先通过串口或其它可靠方式把新的
`res/arcs-mini/boot.bin` 一次性写入 `boot@0x000000`；之后才可依赖修复后的 ADB 下载流程。

### 8.1 recovery 可进入但 push 失败的诊断与保护

`adb_download.ps1` 和 `adb_download.sh` 现在使用 boot raw-flash 接口的完整路径：
`/RAW/NAND/<起始地址>/<镜像字节数十六进制>`。显式传入长度后，boot 会拒绝超出预期长度的
写入，也能检测中途丢包或传输不完整。

每台设备在写入前先执行 `info reason` 和 `info flash` 预检；每个分区写入后，用
`md5sum /RAW/NAND/<起始地址>/<长度>` 回读计算并与本地镜像 MD5 比较。只有所有分区校验通过
才执行重启；任一阶段失败都保持在 recovery，避免失败后自动启动不完整镜像。

预检还会把 1 字节探测文件推送到 `/NULL/adb-sync-probe`。该路径只丢弃数据，不擦写 Flash；
如果此步失败，说明板上仍是缺少 ADB Sync 的旧 boot。量产工具和脚本都会直接提示先用串口
将 `res/arcs-mini/boot.bin` 写到 `0x000000`，避免继续尝试并产生含糊的 raw push 错误。

失败时会自动保存：

- Windows：`tmp/adb-download-failure-<serial>-<timestamp>.log`
- Linux/macOS：`tmp/adb-download-failure-<serial>-<timestamp>.log`，同时保留对应分区的 `tmp/adb-push-*.log`

日志包含失败阶段、ADB push 原始输出、`adb devices -l`、`info reason`、`info flash`
和 `info rawflash`。如果再次下载失败，优先保留这个文件，它可以区分“recovery 只枚举但
未启动 raw-flash”、“flash 擦写失败”和“USB 传输中断”。
