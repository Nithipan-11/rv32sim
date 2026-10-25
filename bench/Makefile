# Build the bare-metal benchmark ELFs with a RISC-V GCC.
CC ?= riscv64-unknown-elf-gcc
OUT ?= ../build/bench
CFLAGS = -march=rv32im_zicsr -mabi=ilp32 -O2 -ffreestanding -fno-builtin -nostdlib -nostartfiles \
         -static -mcmodel=medany -Iruntime -Wall
PROGS = sieve matmul qsort crc32

all: $(addprefix $(OUT)/,$(addsuffix .elf,$(PROGS)))

$(OUT)/%.elf: programs/%.c runtime/crt0.S runtime/rt.c runtime/link.ld | $(OUT)
	$(CC) $(CFLAGS) -T runtime/link.ld runtime/crt0.S runtime/rt.c $< -lgcc -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)
