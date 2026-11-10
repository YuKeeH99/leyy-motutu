# BLE 设备身份与 Wi-Fi 列表

适用于 ARCS-MINI 单 VIF 设备。保留原有配网命令和成功/失败通知。

## iOS / Bleak 连接顺序

1. 设备进入配网后，扫描并连接 `xiaoqiuquan`，发现服务 `E402`。
2. 对 `E403` 开启通知，并等待系统确认订阅成功（iOS 的
   `didUpdateNotificationStateFor` 成功且 `isNotifying == true`；Bleak 等待
   `start_notify` 返回）。`E404` 为只读状态，不用于订阅。
3. 设备会依次推送 `device_info` 和 `wifi_scan`。`device_info` 包含 `chip_id`、`sn`、`firmware_version`。
   客户端也可发送 `A014` 重读身份；发送 `A015` 请求刷新列表。两命令的 payload 为空。
4. 收齐同一 `scan_id` 的所有 JSON 页后展示列表。输入 SSID/密码仍按
   `A001 START` → `A002 SSID` → `A003 PASSWORD` → `A010 DONE` 发送。
5. 只有收到原成功状态 `04 01` 或 `event=wifi_connected` 才视为联网成功。
   失败仍使用 custom payload `0A 01 reason`：`01` 无热点、`02` 密码错误、
   `03` DHCP/IP 失败、`FF` 其他失败。失败后用户可重新提交。

命令写入使用 write-with-response。已有 `A012 AUTH_INFO` 与 `A013 SKIP_WIFI` 保持兼容。
新客户端若等待列表超过 12 秒，可提示超时并允许手动输入；旧固件不实现 `A015`。

## 传输分片

所有数值为字节；多字节命令头为小端序。每个 GATT 数据包最多 20 字节。

- 请求：`index, total, length, data...`。第一片的 data 为
  `E4 03, opcode_le16, payload_length_le16, payload...`；第一片容纳 11 字节
  payload，后续片各 17 字节。空载荷 `A015` 请求为 `01 01 06 E4 03 15 A0 00 00`。
- 自定义通知：`index, total, length, payload_chunk...`，没有命令头；每片载荷
  最多 17 字节，index 从 1 开始。每个完整 JSON 页独立分片。
- 两字节通知是旧状态码，不属于 JSON 分片，可以出现在分片之间。

接收方看到 index=1 应清空旧重组状态；其余分片必须 total 相同且 index 连续递增。
长度不合法、漏片或乱序应丢弃当前消息，等待下一条 index=1，不能拼接旧消息片段。
设备所有自定义通知由同一 worker 顺序发送，片间隔 20ms；断开、取消订阅、休眠
都会令旧任务停止发片，重连后旧连接代次的任务不会在新连接继续发送。

## 设备身份与版本（2026-09-17 扩展）

```json
{"code":0,"event":"device_info","chip_id":"0011223344556677","sn":"设备SN","firmware_version":"2.6.3"}
```

配网成功的 `wifi_connected` 使用相同身份及版本字段。`firmware_version` 来自设备编译的
`PROJECT_VERSION_STR`，不是量产工具版本、Excel 字段或 OTA 待升级版本。旧设备可能缺少此字段，
APP 应显示“未提供”，不得因此拒绝原有配网流程。新增字段不改变通知分片格式。

## 列表内容

示例是一条完整 JSON 消息（实际在 E403 上分多片发送）：

```json
{"event":"wifi_scan","scan_id":12,"page":1,"pages":1,"code":0,"status":"cached","band":"2.4GHz","aps":[{"ssid":"家庭WiFi","rssi":-43,"secured":true},{"ssid":"Cafe","rssi":-65,"secured":false}]}
```

`scan_id` 标识本次响应；`page` 从 1 开始；每页最多 6 个热点，最多 24 个热点。
空结果也发一页 `aps:[]`。SSID 为 UTF-8 JSON 字符串（引号和控制字符按 JSON 转义）。
`rssi` 单位 dBm；`secured` 表示是否加密，不承诺能用个人密码连接企业认证网络。
只列信道 1–14 的 2.4GHz AP；隐藏 SSID 不列出；同 SSID 保留信号最强项，按 RSSI 降序。

| status | 含义 |
| --- | --- |
| fresh | 本次完成 STA 扫描，包括扫描成功但没有热点 |
| cached | 返回已有缓存；SoftAP 期间刷新不会关闭或切换 SoftAP |
| busy | 门户初次扫描尚未完成，或正常 Wi-Fi 管理/连接正在使用无线；可能带上次缓存 |
| failed | 扫描或生成响应失败，code=1；扫描失败保留上次缓存 |
| paused | 设备无线已休眠，禁止发起新扫描；可带已有缓存 |

仅当 STA 已启用、`sys_wifi` 已停止、无门户/连接任务且设备未休眠时，BLE worker
允许直接刷新。该路径不启动无线、不切换模式、不打断正常联网/TTS。SoftAP 配网时
应把“刷新使用缓存”的限制告诉用户；如需隐藏热点或缓存里没有的 SSID，可手动输入。

## 验证边界

仓库测试执行真实 C 扫描/分片函数（驱动和 GATT 边界由测试替身提供）及 Python
通知重组、编号选择、真实配网协程。未连接实机的测试不能证明手机系统的 BLE 吞吐量
或特定路由器认证成功；设备验收需验证订阅、刷新、断连重连、SoftAP 共存与睡眠路径。
