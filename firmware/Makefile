# Makefile - emulator SFR1M44-DU26 clean-room firmware (AT32F415RBT7)
#
# Usage:
#   make            build build/tdsfloppy.elf + .bin + .hex, print size
#   make clean      remove build/
#   make flash-uart flash over CP2102 UART bootloader (BOOT0 high, power-cycle first)
#   make flash-swd  flash bootloader + app via ST-Link + OpenOCD (SWD header J10)
#   make test       host unit tests
#   (make also writes build/UPDATE.UPD, the USB-stick update file: docs/13)
#
# Requires: arm-none-eabi-gcc (Arm GNU Toolchain) + make on PATH.

# Use cmd.exe on Windows explicitly, and a mkdir that tolerates existing dirs.
ifeq ($(OS),Windows_NT)
  SHELL   := cmd.exe
  MKDIR    = if not exist "$(subst /,\,$1)" mkdir "$(subst /,\,$1)"
  RMDIR    = if exist "$(subst /,\,$(BUILD))" rmdir /s /q "$(subst /,\,$(BUILD))"
  COPY     = copy /y "$(subst /,\,$1)" "$(subst /,\,$2)" >nul
else
  MKDIR    = mkdir -p $1
  RMDIR    = rm -rf $(BUILD)
  COPY     = cp $1 $2
endif

TARGET   := tdsfloppy
BUILD    := build
PART     := AT32F415RBT7

# ---- toolchain ----
CC   := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE := arm-none-eabi-size

# ---- paths ----
V        := vendor
CMSIS    := $(V)/cmsis/cm4
DEV      := $(CMSIS)/device_support
DRV      := $(V)/drivers
LDSCRIPT := ld/app.ld          # app at 0x08002000, after the bootloader

INCLUDES := \
  -Iinclude \
  -I$(CMSIS)/core_support \
  -I$(DEV) \
  -I$(DRV)/inc \
  -I$(V)/usb/inc \
  -I$(V)/usb/msc \
  -I$(V)/fatfs

# ---- sources ----
# our code
SRC_C := $(wildcard src/*.c)
# vendor: system file + all peripheral drivers (unused ones cost only compile
# time; linker garbage-collects unreferenced code via --gc-sections)
SRC_C += $(DEV)/system_at32f415.c
SRC_C += $(wildcard $(DRV)/src/*.c)
# USB host core + mass-storage class (Artery), FatFs (ChaN) for the stick
SRC_C += $(wildcard $(V)/usb/src/*.c) $(wildcard $(V)/usb/msc/*.c)
SRC_C += $(V)/fatfs/ff.c $(V)/fatfs/ffunicode.c
SRC_S := $(DEV)/startup/gcc/startup_at32f415.s

OBJ := $(addprefix $(BUILD)/,$(SRC_C:.c=.o) $(SRC_S:.s=.o))

# ---- flags ----
# AT32F415 is Cortex-M4 WITHOUT an FPU (at32f415.h: __FPU_PRESENT 0) -> soft float.
CPU := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
DEFS := -D$(PART) -DUSE_STDPERIPH_DRIVER
# Link-time optimisation of the firmware (not the bootloader); per-file optimisation
# levels are kept. Default from 1.2.0 (docs/06); LTO= builds without it.
LTO ?= -flto
OPT := -O2 -g3   # -Og made track rebuilds too slow for the host's seek-verify

CFLAGS := $(CPU) $(DEFS) $(INCLUDES) $(OPT) \
  -Wall -Wextra -ffunction-sections -fdata-sections -fno-common \
  -std=gnu11 -MMD -MP
ASFLAGS := $(CPU) -x assembler-with-cpp $(DEFS)
LDFLAGS := $(CPU) -T$(LDSCRIPT) -Wl,--gc-sections \
  -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs --specs=nosys.specs \
  -Wl,--print-memory-usage

# ---- rules ----
.PHONY: all clean flash-uart flash-swd debug-server probe test
all: $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex $(BUILD)/boot.bin $(BUILD)/UPDATE.UPD $(BUILD)/$(TARGET)_install.hex
	@$(SIZE) $(BUILD)/$(TARGET).elf $(BUILD)/boot.elf

# Size vs speed: the floppy real-time path (floppy, mfm, buffer, spiflash) is
# -O2; everything else (USB stack, FatFs, drivers, transfers) is -Os, which
# keeps the image well inside the 52 KB update staging area (docs/13).
SPEED_SRC := src/floppy.c src/mfm.c src/buffer.c src/spiflash.c
$(BUILD)/%.o: %.c
	@$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) $(if $(filter $<,$(SPEED_SRC)),,-Os) $(LTO) -c $< -o $@

$(BUILD)/%.o: %.s
	@$(call MKDIR,$(dir $@))
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ) $(LDSCRIPT)
	$(CC) $(OBJ) $(LDFLAGS) $(LTO) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

# ---- bootloader: 8 KB at 0x08000000 (boot/boot.c, include/update.h) ----
BOOT_SRC_C := boot/boot.c src/spiflash.c src/update.c $(DEV)/system_at32f415.c \
  $(DRV)/src/at32f415_crm.c $(DRV)/src/at32f415_gpio.c $(DRV)/src/at32f415_spi.c \
  $(DRV)/src/at32f415_flash.c $(DRV)/src/at32f415_wdt.c
BOOT_OBJ := $(addprefix $(BUILD)/boot/,$(BOOT_SRC_C:.c=.o) $(SRC_S:.s=.o))
BOOT_CFLAGS := $(subst -O2,-Os,$(CFLAGS))

$(BUILD)/boot/%.o: %.c
	@$(call MKDIR,$(dir $@))
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/boot/%.o: %.s
	@$(call MKDIR,$(dir $@))
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/boot.elf: $(BOOT_OBJ) ld/boot.ld
	$(CC) $(BOOT_OBJ) $(CPU) -Tld/boot.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/boot.map \
	  --specs=nano.specs --specs=nosys.specs -Wl,--print-memory-usage -o $@

$(BUILD)/boot.bin: $(BUILD)/boot.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/boot.hex: $(BUILD)/boot.elf
	$(OBJCOPY) -O ihex $< $@

# first installation over USB-DFU (docs/08): bootloader + application in one file
$(BUILD)/$(TARGET)_install.hex: $(BUILD)/boot.hex $(BUILD)/$(TARGET).hex ../tools/mergehex.py
	python ../tools/mergehex.py $@ $(BUILD)/boot.hex $(BUILD)/$(TARGET).hex

# the USB-stick update file (tools/mkupdate.py checks size + link address)
$(BUILD)/UPDATE.UPD: $(BUILD)/$(TARGET).bin include/version.h ../tools/mkupdate.py ../tools/check_image_end.py
	python ../tools/check_image_end.py $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin
	python ../tools/mkupdate.py $(BUILD)/$(TARGET).bin include/version.h $@

clean:
	$(RMDIR)

# host unit tests (native gcc, not the ARM build) - MFM encoder + FAT12 image
.PHONY: test
test:
	gcc -std=gnu11 -Wall -Wextra -DMFM_HOST_TEST -Iinclude -Itest \
	  test/test_mfm.c test/testimg.c src/mfm.c src/fat12.c src/fatimg.c src/update.c src/buttons.c src/status.c -o $(BUILD)/test_mfm.exe
	$(BUILD)/test_mfm.exe
	gcc -std=gnu11 -Wall -Wextra -DBUFFER_HOST_TEST -Iinclude -Itest \
	  test/test_buffer.c src/buffer.c src/fatimg.c src/fat12.c src/update.c -o $(BUILD)/test_buffer.exe
	$(BUILD)/test_buffer.exe

# header-dependency files from -MMD (rebuild an object when a header it uses changes)
-include $(OBJ:.o=.d) $(BOOT_OBJ:.o=.d)

# CP2102 UART bootloader. Set BOOT0 high, power-cycle, then run. Adjust PORT.
# Uses Artery ISP Console if present; see docs/06-build-and-flash.md.
PORT ?= COM3
flash-uart: all
	@echo "Flash $(TARGET).hex over UART on $(PORT) (BOOT0 high + power-cycle first)"
	@echo "  (wire up Artery ISP Console command in docs/06-build-and-flash.md)"

# ST-Link V2 + OpenOCD via SWD header J10. Needs openocd on PATH.
# AT32F415 uses OpenOCD's dedicated 'artery' flash driver (NOT stm32f1x) via
# target/artery/at32f4x.cfg - stm32f1x mis-detects Artery flash. See docs/06.
OCD_IF  := interface/stlink.cfg
OCD_TGT := target/artery/at32f4x.cfg
flash-swd: all
	openocd -f $(OCD_IF) -f $(OCD_TGT) \
	  -c "program $(BUILD)/boot.elf verify" \
	  -c "program $(BUILD)/$(TARGET).elf verify reset exit"
	@$(call COPY,$(BUILD)/$(TARGET).bin,$(BUILD)/flashed.bin)
#	^ record what is on the chip: the SWD tools refuse to run if the build differs

# Start an OpenOCD gdb server for live debugging (leave running; connect with
# arm-none-eabi-gdb build/tdsfloppy.elf -ex 'target extended-remote :3333').
debug-server:
	openocd -f $(OCD_IF) -f $(OCD_TGT)

# Sanity check: just connect, report the core, and release.
probe:
	openocd -f $(OCD_IF) -f $(OCD_TGT) -c "init; reset halt; flash list; reset run; exit"
