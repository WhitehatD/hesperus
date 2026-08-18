######################################################################
# Makefile — STM32 IoT Visual Monitoring Firmware
#
# ARM GCC cross-compilation for B-U585I-IOT02A (STM32U585AI)
#
# Usage:
#   make          Build .elf and .bin
#   make flash    Flash via ST-Link (requires STM32_Programmer_CLI)
#   make clean    Remove build artifacts
#   make size     Report code/data section sizes
######################################################################

# ── Toolchain ─────────────────────────────────────────
PREFIX = arm-none-eabi-
CC     = $(PREFIX)gcc
AS     = $(PREFIX)gcc -x assembler-with-cpp
CP     = $(PREFIX)objcopy
SZ     = $(PREFIX)size
HEX    = $(CP) -O ihex
BIN    = $(CP) -O binary -S

# ── Project ───────────────────────────────────────────
TARGET = hesperus-firmware
BUILD_DIR = build

# ── MCU flags ─────────────────────────────────────────
CPU   = -mcpu=cortex-m33
FPU   = -mfpu=fpv5-sp-d16
FLOAT = -mfloat-abi=hard
MCU   = $(CPU) -mthumb $(FPU) $(FLOAT)

# ── Sources ───────────────────────────────────────────

# Application sources
C_SOURCES  = \
Core/Src/main.c \
Core/Src/debug_log.c \
Core/Src/wifi.c \
Core/Src/camera.c \
Core/Src/mqtt_handler.c \
Core/Src/scheduler.c \
Core/Src/ota_update.c \
Core/Src/wifi_credentials.c \
Core/Src/board_settings.c \
Core/Src/captive_portal.c \
Core/Src/upload_async.c \
Core/Src/cJSON.c \
Core/Src/mx_wifi_hw.c \
Core/Src/stm32u5xx_it.c \
Core/Src/stm32u5xx_hal_msp.c \
Core/Src/system_stm32u5xx.c

# BSP Board sources (only what we need)
C_SOURCES += \
Drivers/BSP/B-U585I-IOT02A/b_u585i_iot02a.c \
Drivers/BSP/B-U585I-IOT02A/b_u585i_iot02a_bus.c \
Drivers/BSP/B-U585I-IOT02A/b_u585i_iot02a_camera.c

# BSP Component sources
C_SOURCES += \
Drivers/BSP/Components/ov5640/ov5640.c \
Drivers/BSP/Components/ov5640/ov5640_reg.c \
Drivers/BSP/Components/mx_wifi/mx_wifi.c \
Drivers/BSP/Components/mx_wifi/io_pattern/mx_wifi_spi.c \
$(wildcard Drivers/BSP/Components/mx_wifi/core/*.c)

# HAL sources (exclude templates which have conflicting weak symbols)
HAL_SOURCES = $(wildcard Drivers/STM32U5xx_HAL_Driver/Src/*.c)
HAL_SOURCES := $(filter-out %_template.c,$(HAL_SOURCES))
C_SOURCES += $(HAL_SOURCES)

# Startup assembly
ASM_SOURCES = \
Drivers/CMSIS/Device/ST/STM32U5xx/Source/Templates/gcc/startup_stm32u585xx.s

# ── Include Paths ─────────────────────────────────────
C_INCLUDES = \
-ICore/Inc \
-IDrivers/BSP/B-U585I-IOT02A \
-IDrivers/BSP/Components/Common \
-IDrivers/BSP/Components/ov5640 \
-IDrivers/BSP/Components/mx_wifi \
-IDrivers/BSP/Components/mx_wifi/core \
-IDrivers/BSP/Components/mx_wifi/io_pattern \
-IDrivers/STM32U5xx_HAL_Driver/Inc \
-IDrivers/STM32U5xx_HAL_Driver/Inc/Legacy \
-IDrivers/CMSIS/Include \
-IDrivers/CMSIS/Device/ST/STM32U5xx/Include

# ── Defines ───────────────────────────────────────────
GIT_COMMIT_COUNT := $(shell git rev-list --count HEAD)
GIT_HASH := $(shell git rev-parse --short HEAD)
# BUILD_EPOCH: injected by CI ("make BUILD_EPOCH=...") so the binary and the
# server upload share the exact same timestamp.  Falls back to current time
# for local developer builds.
BUILD_EPOCH       ?= $(shell date +%s)
FW_VER_STR        := 1.0.$(GIT_COMMIT_COUNT).$(BUILD_EPOCH)

# MQTT broker credentials — overridable at build time ("make MQTT_USERNAME=...
# MQTT_PASSWORD=..."), same mechanism as BUILD_EPOCH. Empty defaults match
# firmware_config.h's own #ifndef fallback (anonymous/dev). NEVER hardcode a
# real password here — CI passes it from GitHub Secrets, local dev builds
# stay anonymous by default.
MQTT_USERNAME        ?=
MQTT_PASSWORD        ?=
FIRMWARE_UPLOAD_TOKEN ?=

C_DEFS = \
-DSTM32U585xx \
-DUSE_HAL_DRIVER \
-DFW_VERSION=\"$(FW_VER_STR)\" \
-DMQTT_USERNAME=\"$(MQTT_USERNAME)\" \
-DMQTT_PASSWORD=\"$(MQTT_PASSWORD)\" \
-DFIRMWARE_UPLOAD_TOKEN=\"$(FIRMWARE_UPLOAD_TOKEN)\"

# Force rebuild when version changes (incremental builds miss -D flag changes)
VERSION_FILE := $(BUILD_DIR)/.fw_version
$(shell mkdir -p $(BUILD_DIR))
$(shell echo -n "$(FW_VER_STR)" > $(VERSION_FILE).tmp)
$(shell cmp -s $(VERSION_FILE).tmp $(VERSION_FILE) 2>/dev/null || mv $(VERSION_FILE).tmp $(VERSION_FILE))
$(shell rm -f $(VERSION_FILE).tmp)

# ── Compiler Flags ────────────────────────────────────
OPT = -O2
CFLAGS  = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT)
CFLAGS += -Wall -Wextra -Wno-unused-parameter
CFLAGS += -fdata-sections -ffunction-sections
CFLAGS += -std=gnu11

# ── Assembler Flags ───────────────────────────────────
ASFLAGS = $(MCU) $(C_DEFS) -Wall -fdata-sections -ffunction-sections

# ── Linker Flags ──────────────────────────────────────
LDSCRIPT = STM32CubeIDE/STM32U585AIIX_FLASH.ld
LDFLAGS  = $(MCU) -specs=nano.specs -T$(LDSCRIPT)
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -lc -lm -lnosys

######################################################################
# Build Rules
######################################################################

# Object files
OBJECTS  = $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(ASM_SOURCES:.s=.o))

# Dependency files
DEPS = $(OBJECTS:.o=.d)

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin size budget

# Release build with strict warnings (BLD-01) and reduced logging (PERF-01)
release: CFLAGS += -DSYSTEM_LOG_LEVEL=1
release: all

budget: $(BUILD_DIR)/$(TARGET).bin
	@python -c "import os, sys; sz=os.path.getsize('$<'); pct=sz*100.0/1048576.0; print(f'Flash Budget: {sz} bytes ({pct:.1f}%% of 1MB bank)'); sys.exit(1 if pct>85.0 else 0)"

# C sources (depend on version file to force rebuild on version change)
$(BUILD_DIR)/%.o: %.c $(VERSION_FILE)
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -MMD -MP -MF"$(@:%.o=%.d)" -o $@ $<

# ASM sources
$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) -c $(ASFLAGS) -o $@ $<

# Link
$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

# Binary
$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(BIN) $< $@

# Intel HEX
$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(HEX) $< $@

######################################################################
# Utility Targets
######################################################################

size: $(BUILD_DIR)/$(TARGET).elf
	$(SZ) $<

flash: $(BUILD_DIR)/$(TARGET).bin
	"C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe" -c port=SWD -w $< 0x08000000 -v -rst

clean:
	rm -rf $(BUILD_DIR)

# Include auto-generated dependencies
-include $(DEPS)

.PHONY: all clean flash size release budget
