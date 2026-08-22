################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Profile/battservice.c \
../Profile/devinfoservice.c \
../Profile/hiddev.c \
../Profile/hidkbdservice.c \
../Profile/scanparamservice.c 

C_DEPS += \
./Profile/battservice.d \
./Profile/devinfoservice.d \
./Profile/hiddev.d \
./Profile/hidkbdservice.d \
./Profile/scanparamservice.d 

OBJS += \
./Profile/battservice.o \
./Profile/devinfoservice.o \
./Profile/hiddev.o \
./Profile/hidkbdservice.o \
./Profile/scanparamservice.o 

DIR_OBJS += \
./Profile/*.o \

DIR_DEPS += \
./Profile/*.d \

DIR_EXPANDS += \
./Profile/*.253r.expand \


# Each subdirectory must supply rules for building sources it contributes
Profile/%.o: ../Profile/%.c
	@	riscv-wch-elf-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Startup" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/APP/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/Profile/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Ld" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/RVMSIS" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

