# User extension: register CLI source files.
# Do NOT edit auto-generated obj/APP/subdir.mk; this file is included
# via makefile.defs (which obj/makefile already pulls in with -include).
# Paths are relative to obj/ (the make working directory).
# The pattern rule "APP/%.o: ../APP/%.c" in obj/APP/subdir.mk covers these.

C_SRCS += \
../APP/cli.c \
../APP/cli_uart.c \
../APP/cli_app_cmds.c \
../APP/cli_os_compat.c

C_DEPS += \
./APP/cli.d \
./APP/cli_uart.d \
./APP/cli_app_cmds.d \
./APP/cli_os_compat.d

OBJS += \
./APP/cli.o \
./APP/cli_uart.o \
./APP/cli_app_cmds.o \
./APP/cli_os_compat.o
