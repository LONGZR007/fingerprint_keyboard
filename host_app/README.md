# 指纹键盘上位机（PyQt5）

基于串口 CLI 的指纹键盘桌面控制程序，支持：

- 串口选择 / 波特率设置 / 连接断开
- 连接后自动初始化：读取 50 个用户信息 + 指纹索引表
- 用户表：ID(0-49)、用户名、发送通道、用户数据(密码)、指纹状态
- 密码默认掩码显示（`*`），点击小眼睛可切换明文
- 设置用户信息（用户名 / 通道 usb|ble|both / 用户数据）
- 录入指纹（自动执行 取消 → 录入 → 切回验证 流程）
- 删除用户（同时删除指纹 + 用户信息）
- 解除 BLE 配对并重新进入可配对广播（`bond clear`）
- 命令行控制台：可手动发送任意 CLI 命令，密码自动掩码

## 安装

```bash
pip install -r requirements.txt
# 或
pip install PyQt5 pyserial
```

## 运行

```bash
python main.py
```

## 使用说明

### 连接
1. 选择串口（点击"刷新"可重新枚举），选择波特率（设备默认 115200）
2. 点击"连接"，连接成功后自动读取全部用户与指纹索引表
3. 左下角命令行控制台可随时手动输入命令（如 `fp_status`、`user count`、`bond clear`）

### 设置用户信息
点击对应行"设置信息"按钮，在弹出的对话框中填写：
- 用户名：≤15 字节
- 发送通道：none / ble / usb / both
- 用户数据（密码）：≤110 字节，**不能包含空格**

确定后发送 `user set <id> <name> <ch> <data>`。

### 录入指纹
点击"录入指纹"，按状态栏提示依次按压 / 移开手指（共 3 次）。
上位机自动执行：

```
fp_cancel      → 等待取消完成
fp_enroll <id> → 等待步骤提示与注册成功/失败
fp_verify      → 切回 1:N 验证状态
```

### 删除
点击"删除"，确认后同时执行 `fp_delete <id>`（删指纹）与 `user del <id>`（删用户信息）。

### 解除 BLE 配对
点击右上角"解除BLE配对"，确认后发送 `bond clear`，设备将解除所有配对、断开连接并重新进入可配对广播。

## 指纹状态机说明（与固件约束一致）

固件上电默认处于 `FP_VERIFY`（1:N 验证轮询）状态，只有 `FP_IDLE` 才能启动
录入/删除/清空/读索引表等操作。因此上位机所有指纹操作都遵循：

```
VERIFY(默认) → fp_cancel → 确认取消 → 执行操作 → fp_verify(恢复验证)
```

## 目录结构

```
host_app/
├── main.py            入口
├── main_window.py     主窗口：连接栏、用户表格、命令行控制台
├── serial_client.py   串口枚举与后台读写线程（按行分帧、回显识别）
├── device_api.py      命令封装与伪同步等待（QEventLoop，不冻结 GUI）
├── fp_controller.py   指纹流程状态机（cancel→操作→verify、防重入）
├── parser.py          固件输出解析纯函数（user get / [FP] / 索引表）
└── requirements.txt
```

## 固件命令速查（设备 CLI）

| 命令 | 说明 |
| --- | --- |
| `fp_enroll <id>` | 录入指纹到 ID (0-49) |
| `fp_verify` | 切回 1:N 验证 |
| `fp_delete <id>` | 删除指定指纹 |
| `fp_clear` | 清空全部指纹 |
| `fp_cancel` | 取消当前操作 |
| `fp_status` | 查询状态 |
| `fp_read_index` | 读取 128 字节索引表 |
| `user set <id> <name> <ch> <data>` | 设置用户（ch: none/ble/usb/both） |
| `user get <id>` / `user del <id>` | 读取 / 删除用户 |
| `user list` / `user count` | 列出 / 统计用户 |
| `bond clear` | 解除 BLE 配对并重新进入可配对广播 |
| `channel <ble\|usb\|both>` | 默认 HID 输出通道 |
