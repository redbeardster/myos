# MyOS x86-64 build (Limine higher-half)

CC = gcc
AS = nasm
LD = ld

CFLAGS = -m64 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
         -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
         -Wall -Wextra -std=gnu11 -I.
LDFLAGS = -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T linker.ld
ASFLAGS = -f elf64

KERNEL_OBJS = kernel.o console.o gdt.o interrupt.o keyboard.o lwkt.o uthread.o msgport.o \
              memory.o vmm.o syscall.o user.o elf.o exec.o proc.o shell_embed.o hello_embed.o isr.o

.PHONY: all run clean iso user

all: myos.iso

user:
	$(MAKE) -C user

kernel.elf: $(KERNEL_OBJS) linker.ld user/shell.elf user/hello.elf
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o kernel.elf

kernel.o: kernel.c limine.h console.h gdt.h interrupt.h keyboard.h lwkt.h memory.h exec.h syscall.h vmm.h
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

console.o: console.c console.h font.h
	$(CC) $(CFLAGS) -c console.c -o console.o

gdt.o: gdt.c gdt.h
	$(CC) $(CFLAGS) -c gdt.c -o gdt.o

interrupt.o: interrupt.c interrupt.h io.h console.h keyboard.h lwkt.h
	$(CC) $(CFLAGS) -c interrupt.c -o interrupt.o

keyboard.o: keyboard.c keyboard.h io.h lwkt.h
	$(CC) $(CFLAGS) -c keyboard.c -o keyboard.o

lwkt.o: lwkt.c lwkt.h console.h interrupt.h
	$(CC) $(CFLAGS) -c lwkt.c -o lwkt.o

uthread.o: uthread.c uthread.h lwkt.h proc.h user.h
	$(CC) $(CFLAGS) -c uthread.c -o uthread.o

msgport.o: msgport.c msgport.h lwkt.h console.h
	$(CC) $(CFLAGS) -c msgport.c -o msgport.o

memory.o: memory.c memory.h console.h limine.h
	$(CC) $(CFLAGS) -c memory.c -o memory.o

vmm.o: vmm.c vmm.h memory.h
	$(CC) $(CFLAGS) -c vmm.c -o vmm.o

syscall.o: syscall.c syscall.h console.h exec.h keyboard.h lwkt.h memory.h msgport.h user.h
	$(CC) $(CFLAGS) -c syscall.c -o syscall.o

user.o: user.c user.h memory.h vmm.h gdt.h lwkt.h
	$(CC) $(CFLAGS) -c user.c -o user.o

elf.o: elf.c elf.h memory.h vmm.h
	$(CC) $(CFLAGS) -c elf.c -o elf.o

exec.o: exec.c exec.h console.h elf.h memory.h myos_abi.h user.h uthread.h proc.h limine.h
	$(CC) $(CFLAGS) -c exec.c -o exec.o

proc.o: proc.c proc.h console.h uthread.h
	$(CC) $(CFLAGS) -c proc.c -o proc.o

isr.o: isr.asm
	$(AS) $(ASFLAGS) isr.asm -o isr.o

user/shell.elf user/hello.elf:
	$(MAKE) -C user

hello_embed.o: user/hello.elf
	$(LD) -r -b binary user/hello.elf -o hello_embed.o

shell_embed.o: user/shell.elf
	$(LD) -r -b binary user/shell.elf -o shell_embed.o

myos.iso: kernel.elf user/shell.elf user/hello.elf limine.conf
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	mkdir -p iso_root/EFI/BOOT
	cp kernel.elf iso_root/boot/kernel
	cp user/shell.elf iso_root/boot/shell.elf
	cp user/hello.elf iso_root/boot/hello.elf
	cp limine.conf iso_root/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o myos.iso
	./limine/limine bios-install myos.iso
	rm -rf iso_root

run: myos.iso
	qemu-system-x86_64 -M q35 -m 256M -cdrom myos.iso -boot d -serial stdio

clean:
	rm -f *.o kernel.elf myos.iso myos.bin boot.o kernel.o
	rm -rf iso_root
	$(MAKE) -C user clean

iso: myos.iso
