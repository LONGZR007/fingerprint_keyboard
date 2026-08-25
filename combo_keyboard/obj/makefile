################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################
-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include StdPeriphDriver/subdir.mk
-include Startup/subdir.mk
-include RVMSIS/subdir.mk
-include Profile/subdir.mk
-include HAL/subdir.mk
-include APP/subdir.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(ASM_DEPS)),)
-include $(ASM_DEPS)
endif
ifneq ($(strip $(ASM_UPPER_DEPS)),)
-include $(ASM_UPPER_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

# Add inputs and outputs from these tool invocations to the build variables 
SECONDARY_FLASH += \
combo_keyboard.hex \

SECONDARY_LIST += \
combo_keyboard.lst \

SECONDARY_SIZE += \
combo_keyboard.siz \


# All Target
all: 
	$(MAKE) --no-print-directory main-build 

main-build: combo_keyboard.elf secondary-outputs

# Tool invocations
combo_keyboard.elf: $(OBJS) $(USER_OBJS_ESCAPE)
	@	riscv-wch-elf-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -T "../Ld/Link.ld" -nostartfiles -Xlinker --gc-sections -L".." -L"../LIB" -L"../StdPeriphDriver" -Xlinker --print-memory-usage -Wl,-Map,"combo_keyboard.map" -o "combo_keyboard.elf" $(OBJS) $(LIBS)
combo_keyboard.hex: combo_keyboard.elf
	@	riscv-wch-elf-objcopy -O ihex "combo_keyboard.elf" "combo_keyboard.hex"
combo_keyboard.lst: combo_keyboard.elf
	@	riscv-wch-elf-objdump --source --all-headers --demangle --line-numbers --wide "combo_keyboard.elf" > "combo_keyboard.lst"
combo_keyboard.siz: combo_keyboard.elf
	riscv-wch-elf-size --format=berkeley "combo_keyboard.elf"

# Other Targets
clean:
	-$(RM) $(DIR_OBJS) $(SECONDARY_FLASH)$(SECONDARY_LIST)$(SECONDARY_SIZE) combo_keyboard.elf
	-$(RM) $(DIR_EXPANDS) $(CALLGRAPH_DOT)
	-$(RM) $(DIR_DEPS)
secondary-outputs: $(SECONDARY_FLASH) $(SECONDARY_LIST) $(SECONDARY_SIZE)

.PHONY: all clean dependents

-include ../makefile.targets
