################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Profile/devinfoservice.c \
../Profile/gattprofile.c 

C_DEPS += \
./Profile/devinfoservice.d \
./Profile/gattprofile.d 

OBJS += \
./Profile/devinfoservice.o \
./Profile/gattprofile.o 

DIR_OBJS += \
./Profile/*.o \

DIR_DEPS += \
./Profile/*.d \

DIR_EXPANDS += \
./Profile/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
Profile/%.o: ../Profile/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Startup" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/Peripheral/APP/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/Peripheral/Profile/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/HAL/include" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/Ld" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/BLE/LIB" -I"/home/long/github/fingerprint_keyboard/sdk/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

