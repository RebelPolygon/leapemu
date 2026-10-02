# Build homebrew cartridges. Usage: make [PROGRAMS=hello] [ARC_PREFIX=...]
ARC_PREFIX ?= ../external/arc-gnu/bin/arc-elf32-
CC      := $(ARC_PREFIX)gcc
OBJCOPY := $(ARC_PREFIX)objcopy
CFLAGS  := -mcpu=arc600_mul64 -O2 -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -Isdk -mno-sdata
LDFLAGS := -T sdk/link.ld -nostdlib -Wl,--gc-sections
OUT     := ../build/homebrew
PROGRAMS ?= hello

all: $(PROGRAMS:%=$(OUT)/%.bin)

$(OUT)/%.elf: %/main.c sdk/crt0.S sdk/link.ld sdk/leapster.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) sdk/crt0.S $< -o $@ -lgcc

$(OUT)/%.bin: $(OUT)/%.elf sdk/mkcart.py
	$(OBJCOPY) -O binary $< $(OUT)/$*.flat
	python3 sdk/mkcart.py $(OUT)/$*.flat $@ --title "$*"

clean:
	rm -rf $(OUT)
.PRECIOUS: $(OUT)/%.elf
.PHONY: all clean
