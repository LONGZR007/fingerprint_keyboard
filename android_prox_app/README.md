# ProxLock —— CH583 键盘「手机离开自动锁屏」App

零依赖（仅 AndroidX + Compose 官方库）的极简守护 App。与固件端 Proximity
Service 配合：手机常驻连接键盘，键盘持续读取 RSSI 判断手机是否随身携带，
离开时自动向电脑发送 Win+L 锁屏。距离判定全部在键盘固件端完成，
App 只负责保持连接 + 显示 RSSI/状态。

## 一、打包 APK（零基础三步）

1. **安装 Android Studio**
   官网下载最新版（自带 JDK 和 Gradle，无需其他配置）：
   https://developer.android.com/studio

2. **导入工程**
   打开 Android Studio → `Open` → 选择本目录（`android_prox_app`）→
   等待右下角 Gradle Sync 完成（首次会自动下载依赖，需要网络）。

3. **生成 APK**
   菜单 `Build` → `Build App Bundle(s) / APK(s)` → `Build APK(s)` →
   完成后右下角弹窗点 `locate`，得到
   `app/build/outputs/apk/debug/app-debug.apk`，
   用微信/数据线/网盘传到手机安装（允许未知来源安装）。

> 手机要求：Android 8.0+，且支持蓝牙。

## 二、首次使用流程

1. 键盘刷入新固件（combo_keyboard.hex），电脑正常连键盘（BLE 或 USB 均可）。
2. 键盘串口（115200，或 USB CDC）执行：

   ```
   prox pair          # 开放 60 秒配对窗口
   ```

3. 手机系统蓝牙设置里搜索并配对 `HID Keyboard`（Just Works，无需输码）。
4. 打开 ProxLock App → 点「开始守护」→ 首次弹出权限全部允许。
5. App 显示「守护中 · -xx dBm」即成功。此后 App 会自动重连，
   键盘离开自动锁屏功能处于 armed 状态。

## 三、校准与配置（键盘串口 CLI）

| 命令 | 说明 |
|---|---|
| `rssi` | 显示手机(EMA)/电脑链路 RSSI 与判定状态 |
| `prox show` | 显示全部配置与状态 |
| `prox on/off` | 功能开关（持久化） |
| `prox thr -70` | 触发阈值（低于该值确认 5s 后锁屏） |
| `prox exit -62` | 恢复阈值（迟滞 8dB 防抖） |
| `prox confirm 5` | 持续确认秒数 |
| `prox cd 60` | 触发后冷却秒数 |
| `prox lost 0` | 手机失联视为离开（0=关闭，防 App 被杀误锁） |
| `prox ch auto` | 锁屏通道 auto/ble/usb/both |
| `prox test` | 手动发一次 Win+L 验证链路 |
| `prox pair` | 开放 60s 配对窗口（新手机配对用） |

校准建议：手机放身上正常姿势，看 `rssi` 读数（约 -60 ~ -70），
把 `thr` 设为比典型值低 5~8dB。

## 四、工作原理（与固件的协议契约）

```
Service  a5f5aa00-c263-4a0c-8e8f-9c0b7a5d3e01
Control  a5f5aa01-...  Write 0x01 激活 / 0x00 停止（需加密链路）
Status   a5f5aa02-...  Notify [int8 rssi][state][flags]
                       state: 0=armed 1=triggered 2=cooldown 3=off
```

- App 连接后先订阅 Status notify，再写 Control 0x01 激活监测连接。
- 键盘每 500ms 读一次手机连接 RSSI，EMA 平滑后 notify 给 App 显示。
- 断线后 App 以 1→2→4→8→16→30s 指数退避自动重连。

## 五、常见问题

- **连不上 / 立刻断开**：键盘白名单只放行已配对设备，先 `prox pair`
  再完成系统配对；配对信息清除后需要重来。
- **通知栏常驻消失**：厂商系统杀后台，请在系统设置中允许「无限制」
  电池与自启动；`prox lost` 建议保持 0，避免误锁。
- **RSSI 一直很低**：检查天线朝向/遮挡；用 `rssi` 命令现场校准阈值。
