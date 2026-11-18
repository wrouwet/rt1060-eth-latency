# Build, flash and monitor the eth_frames application for the NXP MIMXRT1060 EVK.
# Run `make help` for usage.

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
WS   := $(abspath $(REPO)/..)

# REV is the board revision: A, B or C. The bench board is B (silkscreen rev B1).
REV    ?= B
BOARD  ?= mimxrt1060_evk@$(REV)/mimxrt1062/qspi
PORT   ?= /dev/ttyACM0
RUNNER ?= pyocd

export PATH := $(WS)/.venv/bin:$(PATH)
export ZEPHYR_BASE := $(WS)/zephyr
export ZEPHYR_SDK_INSTALL_DIR := $(WS)/.tools/zephyr-sdk-1.0.1

APP_PATH  := $(REPO)/apps/eth_frames
BUILD_DIR := $(REPO)/build/eth_frames-$(subst /,_,$(subst @,_,$(BOARD)))

.DEFAULT_GOAL := help
.PHONY: help setup patches build flash monitor menuconfig clean

help:
	@echo "First-time setup:"
	@echo "  sudo scripts/install-host-deps.sh   Install Ubuntu packages and udev rule"
	@echo "  make setup                          Install Python, west, Zephyr, toolchain; apply patches"
	@echo
	@echo "Daily use:"
	@echo "  make build                          Build [REV=A|B|C]"
	@echo "  make flash                          Build and flash [RUNNER=pyocd|jlink|linkserver]"
	@echo "  make monitor                        Open the serial console [PORT=$(PORT)]"
	@echo "  make menuconfig                     Edit the Kconfig for the build"
	@echo "  make clean                          Delete all build output"

setup:
	$(REPO)/scripts/bootstrap.sh

patches:
	$(REPO)/scripts/apply-patches.sh

build: patches
	cd $(WS) && west build -b $(BOARD) -d $(BUILD_DIR) $(APP_PATH)

flash: build
	cd $(WS) && west flash -d $(BUILD_DIR) --runner $(RUNNER)

monitor:
	picocom -b 115200 $(PORT)

menuconfig:
	cd $(WS) && west build -d $(BUILD_DIR) -t menuconfig

clean:
	rm -rf $(REPO)/build
