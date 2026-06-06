# ==============================================================================
# Makefile для MSYS2 (UCRT64 Environment)
# Bootloader: PE32+ (UEFI) -> Target: x86_64-pc-windows-msvc
# Kernel:     ELF64 (Higher-Half) -> Target: x86_64-unknown-none-elf
# ==============================================================================

CC_KERN   = clang
LD_KERN   = ld.lld
CC_BOOT   = clang
LD_BOOT   = clang

# --- Флаги Ядра (ELF64, Higher-Half, SysV ABI) ---
# -fno-builtin: запрещает компилятору подставлять printf/memset и т.д. из libc
CFLAGS_KERN  = -target x86_64-unknown-none-elf -ffreestanding -fno-builtin \
               -mno-red-zone -mcmodel=kernel -fno-pie -O2 -Wall -Wextra -std=c11
LDFLAGS_KERN = -m elf_x86_64 -nostdlib -T kernel.ld

# --- Флаги Bootloader (PE32+, UEFI, MS ABI) ---
# -mno-stack-arg-probe: КРИТИЧНО! Отключает эмиссию __chkstk для MS ABI
# -fno-stack-check/-protector: отключает stack cookies и проверки границ
CFLAGS_BOOT  = -target x86_64-pc-windows-msvc -ffreestanding -fno-builtin \
               -mno-red-zone -fno-stack-check -fno-stack-protector -mno-stack-arg-probe \
               -O2 -Wall -Wextra -std=c11
# Убрано -shared: UEFI приложения это стандартные PE32+ exe, а не DLL
LDFLAGS_BOOT = -nostdlib -fuse-ld=lld -Wl,/subsystem:efi_application -Wl,/entry:efi_main -Wl,/fixed-base:no

OBJS_KERN = boot_stub.o main.o MemoryControl.o InterruptControl.o
OBJS_BOOT = bootloader.o

.PHONY: all clean

all: bootloader.efi kernel.bin

# 1. Компиляция файлов Ядра
%.o: %.c
	$(CC_KERN) $(CFLAGS_KERN) -c $< -o $@

# 2. Компиляция Bootloader
bootloader.o: bootloader.c
	$(CC_BOOT) $(CFLAGS_BOOT) -c $< -o $@

# 3. Линковка Ядра
kernel.bin: $(OBJS_KERN)
	$(LD_KERN) $(LDFLAGS_KERN) -o $@ $^

# 4. Линковка Bootloader
bootloader.efi: $(OBJS_BOOT)
	$(LD_BOOT) $(LDFLAGS_BOOT) -o $@ $^

clean:
	rm -f *.o kernel.bin bootloader.efi