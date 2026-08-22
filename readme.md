示例代码路径：sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/obj

# 沙箱编译环境适配方案

为让 CH583 BLE HID_Keyboard 例程在本沙箱（非 WCH MounRiver Studio 环境）通过编译，在未改动工程源码与自动生成 makefile 的前提下，做了以下工具链层适配：

1. **安装替代工具链**：安装 `gcc-riscv64-unknown-elf` + `picolibc-riscv64-unknown-elf`，并建立 `riscv-wch-elf-*` → `riscv64-unknown-elf-*` 软链，替代缺失的 WCH MounRiver 工具链。
2. **路径映射**：建立符号链接 `/home/long/github/fingerprint_keyboard` → `/workspace`，使 makefile 中写死的绝对路径生效。
3. **specs 补全**：编写 `nano.specs` / `nosys.specs`（对接 picolibc，替代 makefile 假定存在的新库 specs，补充 `-lc` / `-ldummyhost`）。
4. **gcc 包装器**：为 `riscv-wch-elf-gcc` 包一层 wrapper，自动将 `-march=rv32imac` 改写为 `rv32imac_zicsr_zifencei`（新版 GCC 需要 zicsr / zifencei 才能汇编 `csrr` / `fence.i`），并补 picolibc 的 `-isystem` / `-L` 路径。

编译结果：0 error，23 warning（均为厂商代码无害告警），生成 `HID_Keyboard.elf` / `HID_Keyboard.hex`。
