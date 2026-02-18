CROSS ?= x86_64-elf
CC := $(CROSS)-gcc
LD := $(CROSS)-ld
AS := nasm

ENABLE_SHELL ?= 0
EXTRA_OBJS ?=
BUILD_DIR ?= build

KERNEL_DIR := kernel
BOOTLOADER_DIR := bootloader

CFLAGS := -ffreestanding -mno-red-zone -fno-pic -mcmodel=large \
	-I $(KERNEL_DIR) -I $(KERNEL_DIR)/drivers -I $(KERNEL_DIR)/fs \
	-DCONFIG_ENABLE_SHELL=$(ENABLE_SHELL)

KERNEL_C_SRCS := \
	$(KERNEL_DIR)/kernel.c \
	$(KERNEL_DIR)/idt.c \
	$(KERNEL_DIR)/isr.c \
	$(KERNEL_DIR)/gdt.c \
	$(KERNEL_DIR)/pmm.c \
	$(KERNEL_DIR)/drivers/ata.c \
	$(KERNEL_DIR)/drivers/keyboard.c \
	$(KERNEL_DIR)/drivers/framebuffer.c \
	$(KERNEL_DIR)/fs/vfs.c \
	$(KERNEL_DIR)/fs/fat32.c \
	$(KERNEL_DIR)/paging.c \
	$(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/elf_loader.c \
	$(KERNEL_DIR)/sched.c \
	$(KERNEL_DIR)/tty.c \
	$(KERNEL_DIR)/font.c \
	$(KERNEL_DIR)/console.c

KERNEL_C_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_C_SRCS))
ENTRY_ASM_OBJ := $(BUILD_DIR)/kernel/entry_asm.o
ISR_ASM_OBJ := $(BUILD_DIR)/kernel/isr_asm.o
SYSCALL_ENTRY_ASM_OBJ := $(BUILD_DIR)/kernel/syscall_entry_asm.o
SWITCH_ASM_OBJ := $(BUILD_DIR)/kernel/switch_asm.o
KERNEL_ASM_ELF_OBJS := $(ENTRY_ASM_OBJ) $(ISR_ASM_OBJ) $(SYSCALL_ENTRY_ASM_OBJ) $(SWITCH_ASM_OBJ)
KERNEL_OBJS := $(KERNEL_C_OBJS) $(KERNEL_ASM_ELF_OBJS)
KERNEL_LINK_OBJS := $(ENTRY_ASM_OBJ) $(ISR_ASM_OBJ) $(SYSCALL_ENTRY_ASM_OBJ) \
	$(KERNEL_C_OBJS) $(SWITCH_ASM_OBJ)

.PHONY: all image clean

all: boot.bin kernel.bin

boot.bin: $(BOOTLOADER_DIR)/boot.asm
	$(AS) -f bin $< -o $@

kernel.bin: $(KERNEL_OBJS) $(KERNEL_DIR)/linker.ld
	$(LD) -T $(KERNEL_DIR)/linker.ld -o $@ $(KERNEL_LINK_OBJS) $(EXTRA_OBJS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/entry_asm.o: $(KERNEL_DIR)/entry.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/isr_asm.o: $(KERNEL_DIR)/isr.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/syscall_entry_asm.o: $(KERNEL_DIR)/syscall_entry.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/switch_asm.o: $(KERNEL_DIR)/switch.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

image: all
	cat boot.bin kernel.bin > phobos.img
	truncate -s 131072 phobos.img

clean:
	rm -rf $(BUILD_DIR) boot.bin kernel.bin phobos.img
