# ARCS-MINI NFC 互认上位机

这是 V2.1 双板 NFC 身份互认的本地网页上位机。它会通过 ADB 实时读取两块板的
`v2hw nfc peer status`，无需安装 Python 第三方包。

## 启动

在仓库根目录执行：

```bash
python3 tools/nfc_peer_dashboard.py
```

脚本会自动打开 <http://127.0.0.1:8765/>。按 `Ctrl+C` 停止上位机。
默认将绿色 `1634` 板标记为“狗子A”，蓝色 `1d41` 板标记为“狗子B”。每次新的
靠近身份交换验证通过时，页面会全屏弹出“已识别身份：狗子A ↔ 狗子B”。

如果不希望自动打开浏览器：

```bash
python3 tools/nfc_peer_dashboard.py --no-browser
```

端口被占用时可更换端口：

```bash
python3 tools/nfc_peer_dashboard.py --port 8877
```

## 一键双板互认

在“双板互认”页点击“一键开始互认”。上位机会先结束旧会话，再严格按以下
顺序启动：

1. 蓝色板 `6b3bc01b1d41cfbd` 作为固定 `Target`。
2. 等待 Target 进入就绪状态后，启动绿色板
   `6b3bc01b1634cfbd` 作为固定 `Reader`。

角色由板端芯片 ID 固定，上位机不会自动交换 Reader/Target，也不会调用
`peer once reader|target` 之类的临时角色命令。

只有双方 `seen/enter` 计数均相对启动前增长，页面才判定为成功。
这两个计数只会在板端校验 CRC 保护的身份包、并确认对端 64 位 ID
与固定角色匹配后增长。因此即使天线只快速碰一下，ADB 轮询到时
`present/peer` 已因离开而清空，上位机仍能根据双方持久计数确认成功。
成功时会强提示“狗子A 已识别 狗子B”并显示上位机确认耗时和时间。
重复 serial 的 ADB 瞬时轮询中断会显示“正在自动重连”，不会把本轮 NFC
测试误判为终止异常。

天线分开约 6 秒、双方报告离开后，页面显示“已离开”。不需要再点按钮；
下一次靠近并完成新的双向身份交换后，会再次计数并弹出成功提示。

## 界面字段

- `已识别对方`：最近 6 秒内完成过有效的双向身份数据交换。
- `对方 64 位 ID`：对方 ARCS SoC 唯一芯片 ID，不是仅 4 字节的 NFC UID。
- `交换成功`：完整身份包的累计验证次数。
- `靠近` / `离开`：本次启动以来的状态切换次数。
- `错误码`：`0` 表示正常。

两块 V2.1 板的 ADB serial 相同，脚本会每次根据 `transport_id` 访问设备，因此重启或
重插 Type-C 后不需要手工修改设备号。

## 卡片测速模式

页面顶部切换到“卡片测速”，选择一块在线开发板作为 Reader，再选择测试目标：

- `CARD-A`：UID `08 A1 00 01`
- `CARD-B`：UID `08 B2 00 02`

点击“开始连续测速”后，上位机会暂停所选板的双板互认服务，并连续调用：

```text
v2hw nfc card-read 1000
```

页面显示当前目标、最新/首次/最快板端扫描耗时、成功次数与超时次数。每次成功会全屏
弹出卡名、UID 和耗时。同一张卡一直贴着只记录一次；移开卡片，界面重新显示“等待靠近”
后才会记录下一次。点击“停止并恢复互认”或退出上位机时，如果进入测速前
peer 服务原本在运行，会执行 `v2hw nfc peer start` 恢复原来的固定双板角色；
原本未启动的 peer 不会被上位机擅自启动。

测速使用板端机器行，不把 ADB 进程开销计入延迟：

```text
NFC_CARD_SCAN event=found card=CARD-A uid=08A10001 elapsed_ms=23 scan_status=0 peer_restored=0 restore_status=0
NFC_CARD_SCAN event=timeout card=NONE uid=NONE elapsed_ms=1000 scan_status=-7 peer_restored=0 restore_status=0
```

可通过启动参数修改单次扫描窗口，范围会被限制在 `100~5000ms`：

```bash
python3 tools/nfc_peer_dashboard.py --card-timeout-ms 2000
```
