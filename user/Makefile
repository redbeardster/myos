# MyOS userland (MyOS ABI, not Linux)

CC = gcc
AS = nasm
LD = ld

CFLAGS = -m64 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
         -fno-pic -fno-pie -mno-red-zone -Wall -Wextra -std=gnu11 -I.
ASFLAGS = -f elf64
LDFLAGS = -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000

CRT = crt0.o

.PHONY: all clean

all: shell.elf hello.elf

shell.elf: $(CRT) shell.o linker.ld
	$(LD) $(LDFLAGS) -T linker.ld $(CRT) shell.o -o shell.elf

hello.elf: $(CRT) hello.o linker.ld
	$(LD) $(LDFLAGS) -T linker.ld $(CRT) hello.o -o hello.elf

crt0.o: crt0.asm
	$(AS) $(ASFLAGS) crt0.asm -o crt0.o

shell.o: shell.c myos.h ../myos_abi.h
	$(CC) $(CFLAGS) -c shell.c -o shell.o

hello.o: hello.c myos.h ../myos_abi.h
	$(CC) $(CFLAGS) -c hello.c -o hello.o

clean:
	rm -f *.o shell.elf hello.elf
