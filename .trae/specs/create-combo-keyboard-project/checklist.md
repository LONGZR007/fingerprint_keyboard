# Checklist

- [x] `/workspace/combo_keyboard/` 目录结构完整，包含 APP/、Profile/、HAL/、LIB/、Ld/、RVMSIS/、Startup/、StdPeriphDriver/
- [x] `APP/` 包含 cli.c、cli_app_cmds.c、cli_os_compat.c、cli_uart.c、hidkbd.c、hidkbd_main.c、usb_composite.c
- [x] `APP/include/` 包含 cli.h、cli_uart.h、hidkbd.h、usb_composite.h
- [x] USB 设备描述符 class=0xEF, sub=0x02, proto=0x01（IAD 复合设备）
- [x] USB 配置描述符包含 3 个接口：HID 键盘(0x03)、CDC 通信(0x02/0x02)、CDC 数据(0x0A)
- [x] 端点分配正确：EP1=HID IN Interrupt, EP2=CDC 通知 IN Interrupt, EP3=CDC 数据 IN Bulk, EP4=CDC 数据 OUT Bulk
- [x] CDC 功能描述符完整：Header(0x24,0x00) + ACM(0x24,0x02) + Union(0x24,0x06) + CallMgmt(0x24,0x01)
- [x] CDC 类请求处理：SET_LINE_CODING / GET_LINE_CODING / SET_CONTROL_LINE_STATE
- [x] `usb_cdc_send(buf, len)` 能通过 EP3 IN Bulk 发送数据
- [x] `usb_cdc_recv()` 能从 EP4 OUT 接收数据
- [x] `usb_hid_send_key(modifier, keycode)` 能通过 EP1 IN Interrupt 发送 HID 报告
- [x] `usb_hid_send_text(text)` 能将 ASCII 字符串转为 HID 键码并发送
- [x] `hidkbd_main.c` 初始化 BLE（HidEmu_Init）+ USB（usb_composite_init），主循环同时处理 TMOS 和 USB
- [x] `type` 命令同时通过 BLE 和 USB HID 键盘发送字符
- [x] 使用 `makefile.defs` + `subdir_extra.mk` 模式，不直接修改自动生成的 subdir.mk
- [x] 沙箱 `make all` 编译通过，无 error
- [x] ELF 生成成功，FLASH 占用 36.08% (< 448KB)，RAM 占用 100% (= 32KB)
