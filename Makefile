# Makefile
CC = gcc
AS = nasm
LD = ld
CFLAGS = -m32 -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector -c
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib
ASFLAGS = -f elf32

all: myos.bin

boot.o: boot.asm
	$(AS) $(ASFLAGS) boot.asm -o boot.o

kernel.o: kernel.c
	$(CC) $(CFLAGS) kernel.c -o kernel.o

myos.bin: boot.o kernel.o
	$(LD) $(LDFLAGS) boot.o kernel.o -o myos.bin

clean:
	rm -f *.o myos.bin

run: myos.bin
	qemu-system-i386 -kernel myos.bin

# Для красивого запуска без Makefile:
# qemu-system-i386 -kernel myos.bin
