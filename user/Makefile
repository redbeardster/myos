# MyOS userland (MyOS ABI, not Linux)

include programs.mk

CC = gcc
AS = nasm
LD = ld

INC = -I. -I../include
CFLAGS = -m64 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
         -fno-pic -fno-pie -mno-red-zone -Wall -Wextra -std=gnu11 $(INC)
ASFLAGS = -f elf64
LDFLAGS = -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000

CRT = crt0.o
ELFS = $(PROGRAMS:=.elf)

.PHONY: all clean

all: $(ELFS)

%.elf: %.o $(CRT) linker.ld
	$(LD) $(LDFLAGS) -T linker.ld $(CRT) $< -o $@

crt0.o: crt0.asm
	$(AS) $(ASFLAGS) crt0.asm -o crt0.o

%.o: %.c myos.h
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: %.o

clean:
	rm -f *.o $(ELFS)
