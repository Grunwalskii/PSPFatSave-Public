TARGET = pspfatsave

# Debug switch (see pspfatsave.h). Default 1 = development build (UART + MS logging,
# on-screen checkpoints, boot banner). `make DEBUG_BUILD=0` = release: all of that is
# compiled out at the call site (no-op macros), producing a silent, smaller binary.
DEBUG_BUILD ?= 1

all: inc_version
inc_version:
	@powershell -ExecutionPolicy Bypass -File inc_version.ps1
.PHONY: inc_version

# ARK-4 CFW SDK (https://github.com/PSP-Archive/ARK-4) — provides the
# kernel-mode SystemCtrlForKernel headers/lib. Only the minimal files needed
# to build are included (common/include/* and libs/SystemCtrlForKernel/).
ARK4_SDK = ARK-4

PSPSDK_PATH = $(shell psp-config --pspsdk-path)

# Source files in src/plugin, build objects in build/
VPATH = src/plugin:build
OBJS = build/main.o build/fatsave.o build/gfx.o build/debug.o build/overclock.o build/sysstats.o build/videoskip.o build/menu.o build/utils.o build/uart.o build/fastlz_compress.o build/fastlz_decompress.o build/extras.o build/stub.o build/exports.o build/apply.o

$(OBJS): src/plugin/pspfatsave.h src/plugin/version.h

build/%.o: src/plugin/%.c src/plugin/pspfatsave.h src/plugin/version.h
	psp-gcc -I$(PSPSDK_PATH)/include/libc -Isrc/plugin -I$(ARK4_SDK)/common/include -I. -I$(PSPSDK_PATH)/include -O2 -G0 -Wall -fno-builtin-printf -D_PSP_FW_VERSION=660 -DDEBUG_BUILD=$(DEBUG_BUILD) -c -o $@ $<

build/%.o: src/plugin/%.S src/plugin/pspfatsave.h
	psp-gcc -Isrc/plugin -I$(ARK4_SDK)/common/include -I. -I$(PSPSDK_PATH)/include -O2 -G0 -Wall -fno-builtin-printf -DDEBUG_BUILD=$(DEBUG_BUILD) -c -o $@ $<

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

BUILD_PRX = 1
PRX_EXPORTS = src/plugin/exports.exp

INCDIR = src/plugin $(ARK4_SDK)/common/include
CFLAGS = -O2 -G0 -Wall -fno-builtin-printf
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS) -c

LIBDIR = $(ARK4_SDK)/libs/SystemCtrlForKernel
LIBS = -lpspkernel -lc -lpspuser -lpspsystemctrl_kernel -lpsppower -lpsprtc -lpspge -lpspumd -lpsphprm_driver

# We only ever run on Ark-4 6.61 (new CFW) - no need for the PSPSDK's
# 1.50-era default. This matters for at least one struct: pspthreadman_kernel.h's
# SceKernelThreadKInfo only includes the `unk3` field for _PSP_FW_VERSION>=200,
# and the real kernel on 6.61 almost certainly expects that field present,
# shifting every later field (including thContext) if it's compiled out.
PSP_FW_VERSION = 660

PSPSDK=$(PSPSDK_PATH)
include $(PSPSDK)/lib/build_prx.mak
