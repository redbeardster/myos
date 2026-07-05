# MyOS x86-64 build (Limine higher-half)

include user/programs.mk

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

USER_ELFS = $(addprefix user/,$(PROGRAMS:=.elf))
EMBED_OBJS = $(addprefix $(BUILD)/,$(PROGRAMS:=_embed.o))

KERNEL_C = \
	kernel/main.c \
	kernel/arch/x86_64/gdt.c \
	kernel/arch/x86_64/interrupt.c \
	kernel/arch/x86_64/lapic.c \
	kernel/arch/x86_64/smp.c \
	kernel/drivers/console.c \
	kernel/drivers/keyboard.c \
	kernel/sched/lwkt.c \
	kernel/sched/uthread.c \
	kernel/sched/token.c \
	kernel/sched/msgport.c \
	kernel/sched/kbdd.c \
	kernel/mm/memory.c \
	kernel/mm/vmm.c \
	kernel/syscall/syscall.c \
	kernel/syscall/user.c \
	kernel/proc/elf.c \
	kernel/proc/exec.c \
	kernel/proc/proc.c \
	kernel/proc/proc_mutex.c

KERNEL_OBJS = $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C)) \
                $(BUILD)/kernel/arch/x86_64/isr.o \
                $(EMBED_OBJS) \
                $(BUILD)/user_embeds.o

.PHONY: all run clean iso user limine.conf

all: $(BUILD)/myos.iso

user:
	$(MAKE) -C user

limine.conf: limine.conf.in user/programs.mk tools/gen_limine_conf.sh
	chmod +x tools/gen_limine_conf.sh
	./tools/gen_limine_conf.sh $(PROGRAMS) < limine.conf.in > limine.conf

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/arch/x86_64/linker.ld $(USER_ELFS)
	@mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/arch/x86_64/isr.o: kernel/arch/x86_64/isr.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%_embed.o: user/%.elf
	@mkdir -p $(BUILD)
	$(LD) -r -b binary user/$*.elf -o $@

$(BUILD)/user_embeds_gen.c: user/programs.mk tools/gen_user_embeds.sh $(USER_ELFS)
	@mkdir -p $(BUILD)
	chmod +x tools/gen_user_embeds.sh
	./tools/gen_user_embeds.sh $(PROGRAMS) > $@

$(BUILD)/user_embeds.o: $(BUILD)/user_embeds_gen.c
	$(CC) $(CFLAGS) -c $< -o $@

$(USER_ELFS):
	$(MAKE) -C user

$(BUILD)/myos.iso: $(BUILD)/kernel.elf $(USER_ELFS) limine.conf
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine
	mkdir -p $(BUILD)/iso_root/EFI/BOOT
	cp $(BUILD)/kernel.elf $(BUILD)/iso_root/boot/kernel
	$(foreach elf,$(USER_ELFS),cp $(elf) $(BUILD)/iso_root/boot/;)
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
	qemu-system-x86_64 -M q35 -m 256M -smp 2 -cdrom $(BUILD)/myos.iso -boot d -serial stdio

clean:
	rm -rf $(BUILD)
	$(MAKE) -C user clean

iso: $(BUILD)/myos.iso
