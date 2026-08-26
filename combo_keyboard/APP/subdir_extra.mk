# User extension: register CLI + composite USB + main source files.
# Only appends files NOT already present in C_SRCS/OBJS/C_DEPS.
# This prevents duplicate definitions when MRS auto-generates
# obj/APP/subdir.mk with the same files already included.
# Included via makefile.defs (obj/makefile line: -include ../makefile.defs).

_cli_srcs := ../APP/cli.c ../APP/cli_uart.c ../APP/cli_app_cmds.c ../APP/cli_os_compat.c ../APP/usb_composite.c ../APP/keyboard_dispatch.c ../APP/fp_uart.c ../APP/fp_proto.c ../APP/fp_sm.c ../APP/fp_app_cmds.c
_cli_deps := ./APP/cli.d ./APP/cli_uart.d ./APP/cli_app_cmds.d ./APP/cli_os_compat.d ./APP/usb_composite.d ./APP/keyboard_dispatch.d ./APP/fp_uart.d ./APP/fp_proto.d ./APP/fp_sm.d ./APP/fp_app_cmds.d
_cli_objs := ./APP/cli.o ./APP/cli_uart.o ./APP/cli_app_cmds.o ./APP/cli_os_compat.o ./APP/usb_composite.o ./APP/keyboard_dispatch.o ./APP/fp_uart.o ./APP/fp_proto.o ./APP/fp_sm.o ./APP/fp_app_cmds.o

C_SRCS += $(filter-out $(C_SRCS),$(_cli_srcs))
C_DEPS += $(filter-out $(C_DEPS),$(_cli_deps))
OBJS   += $(filter-out $(OBJS),$(_cli_objs))
