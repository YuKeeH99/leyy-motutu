# Python BLE 配网

设备连续按键三次进入配网模式后，Windows 终端在仓库根目录执行：

```powershell
py -3 -m pip install -r .\配网\requirements.txt
py -3 .\配网\ble_provision.py
```

程序会扫描 BLE、列出设备、让用户选择，然后通过 `0xA014` 读取设备
`ChipID`、量产工具写入的灵芯 `SN` 和实际固件版本，显示设备扫描到的 2.4 GHz Wi-Fi 列表
（编号、SSID、RSSI、是否加密）。选择编号并输入密码后，
程序等待设备实际联网成功，不把“配置已接收”误报成“联网成功”。密码使用隐藏输入。
输入 `r` 请求刷新；输入 `m` 可手动配置隐藏 SSID。开放网络无需输入密码。
设备正在提供 SoftAP 配网页面时，列表取自启动热点前的扫描缓存；刷新不会断开手机的
SoftAP 连接。无线正在联网或设备休眠时会提示忙或休眠，并保留可用缓存。

也支持非交互参数：

```powershell
py -3 .\配网\ble_provision.py --device xiaoqiuquan --ssid YOUR_SSID --password YOUR_PASSWORD
```

## 协议

- GATT 服务：`0xE402`
- 写入/通知特征：`0xE403`
- 原有命令：`A001 START`、`A002 SSID`、`A003 PASSWORD`、`A010 DONE`
- 新增只读命令：`A014 DEVICE_INFO`
- 新增扫描请求：`A015 WIFI_SCAN`，返回分页 `wifi_scan` JSON 通知
- `A014` 返回：`{"code":0,"event":"device_info","chip_id":"...","sn":"...","firmware_version":"2.6.3"}`
- 联网成功返回：`{"code":0,"event":"wifi_connected","chip_id":"...","sn":"...","firmware_version":"2.6.3"}`
- `firmware_version` 是设备实际运行应用的版本，不是服务器目标版本。旧固件没有此字段时显示“未提供”，仍允许配网。

Wi-Fi SoftAP 名称为 `MHZW-*`，当前 BLE 完整名称为 `xiaoqiuquan`；Python 工具
优先显示 `xiaoqiuquan`、`ARCS` 与 `MHZW`，也保留其他名称的设备供选择。

完整帧格式、iOS 订阅顺序和缓存状态见 [BLE 热点列表协议](BLE热点列表协议.md)。

无需设备的行为验证：

```powershell
py -3 -m unittest discover -s .\配网 -p "test_ble_*.py"
```

其中固件函数测试使用本机 LISTENAI RISC-V GCC 和模拟器，不烧录设备、不改构建产物。
