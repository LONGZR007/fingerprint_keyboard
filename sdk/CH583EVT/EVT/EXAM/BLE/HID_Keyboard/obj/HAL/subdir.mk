################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/MCU.c \
/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/RTC.c \
/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/SLEEP.c 

C_DEPS += \
./HAL/MCU.d \
./HAL/RTC.d \
./HAL/SLEEP.d 

OBJS += \
./HAL/MCU.o \
./HAL/RTC.o \
./HAL/SLEEP.o 

DIR_OBJS += \
./HAL/*.o \

DIR_DEPS += \
./HAL/*.d \

DIR_EXPANDS += \
./HAL/*.253r.expand \


# Each subdirectory must supply rules for building sources it contributes
HAL/MCU.o: /home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/MCU.c
	@	riscv-wch-elf-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Startup" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/APP/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/Profile/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Ld" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/RVMSIS" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
HAL/RTC.o: /home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/RTC.c
	@	riscv-wch-elf-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Startup" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/APP/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/Profile/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Ld" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/RVMSIS" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
HAL/SLEEP.o: /home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/SLEEP.c
	@	riscv-wch-elf-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Startup" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/APP/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/Profile/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Ld" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/RVMSIS" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

