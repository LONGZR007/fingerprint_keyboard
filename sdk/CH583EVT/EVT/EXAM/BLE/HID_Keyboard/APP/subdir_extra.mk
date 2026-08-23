# User extension: register CLI source files.
# Only appends files NOT already present in C_SRCS/OBJS/C_DEPS.
# This prevents duplicate definitions when MRS auto-generates
# obj/APP/subdir.mk with the same files already included.
# Included via makefile.defs (obj/makefile line: -include ../makefile.defs).

_cli_srcs := ../APP/cli.c ../APP/cli_uart.c ../APP/cli_app_cmds.c ../APP/cli_os_compat.c
_cli_deps := ./APP/cli.d ./APP/cli_uart.d ./APP/cli_app_cmds.d ./APP/cli_os_compat.d
_cli_objs := ./APP/cli.o ./APP/cli_uart.o ./APP/cli_app_cmds.o ./APP/cli_os_compat.o

C_SRCS += $(filter-out $(C_SRCS),$(_cli_srcs))
C_DEPS += $(filter-out $(C_DEPS),$(_cli_deps))
OBJS   += $(filter-out $(OBJS),$(_cli_objs))
