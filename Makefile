# MyOS x86-64 build (Limine higher-half)

CC = gcc
AS = nasm
LD = ld

INC = -Iinclude
BUILD = build

CFLAGS = -m64 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
         -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
         -Wall -Wextra -std=gnu11 $(INC)
LDFLAGS = -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
          -T kernel/arch/x86_64/linker.ld
ASFLAGS = -f elf64

KERNEL_C = \
	kernel/main.c \
	kernel/arch/x86_64/gdt.c \
	kernel/arch/x86_64/interrupt.c \
	kernel/drivers/console.c \
	kernel/drivers/keyboard.c \
	kernel/sched/lwkt.c \
	kernel/sched/uthread.c \
	kernel/sched/msgport.c \
	kernel/mm/memory.c \
	kernel/mm/vmm.c \
	kernel/syscall/syscall.c \
	kernel/syscall/user.c \
	kernel/proc/elf.c \
	kernel/proc/exec.c \
	kernel/proc/proc.c

KERNEL_OBJS = $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C)) \
                $(BUILD)/kernel/arch/x86_64/isr.o \
                $(BUILD)/shell_embed.o \
                $(BUILD)/hello_embed.o

.PHONY: all run clean iso user

all: $(BUILD)/myos.iso

user:
	$(MAKE) -C user

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/arch/x86_64/linker.ld user/shell.elf user/hello.elf
	@mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/arch/x86_64/isr.o: kernel/arch/x86_64/isr.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/shell_embed.o: user/shell.elf
	@mkdir -p $(BUILD)
	$(LD) -r -b binary user/shell.elf -o $@

$(BUILD)/hello_embed.o: user/hello.elf
	@mkdir -p $(BUILD)
	$(LD) -r -b binary user/hello.elf -o $@

user/shell.elf user/hello.elf:
	$(MAKE) -C user

$(BUILD)/myos.iso: $(BUILD)/kernel.elf user/shell.elf user/hello.elf limine.conf
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine
	mkdir -p $(BUILD)/iso_root/EFI/BOOT
	cp $(BUILD)/kernel.elf $(BUILD)/iso_root/boot/kernel
	cp user/shell.elf $(BUILD)/iso_root/boot/shell.elf
	cp user/hello.elf $(BUILD)/iso_root/boot/hello.elf
	cp limine.conf $(BUILD)/iso_root/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin \
		$(BUILD)/iso_root/boot/limine/
	cp limine/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $@
	./limine/limine bios-install $@
	rm -rf $(BUILD)/iso_root

run: $(BUILD)/myos.iso
	qemu-system-x86_64 -M q35 -m 256M -cdrom $(BUILD)/myos.iso -boot d -serial stdio

clean:
	rm -rf $(BUILD)
	$(MAKE) -C user clean

iso: $(BUILD)/myos.iso
