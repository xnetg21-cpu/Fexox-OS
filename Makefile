# Fexox OS x64 - Makefile

CC = gcc
CCFLAGS = -ffreestanding -fno-builtin -fno-stack-protector \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -Wall -Wextra -std=c99 -O2

AS = nasm
ASFLAGS = -f elf64

LD = ld
LDFLAGS = -n

TARGETS = bootloader.o \
          kernel_main.o \
          context_switch.o \
          memory.o \
          ipc.o

all: kernel.elf kernel.iso

# Bootloader (UEFI x64)
bootloader.o: bootloader.c
	@echo "[CC] Building bootloader..."
	$(CC) $(CCFLAGS) -DUEFI -c $< -o $@

# Kernel main
kernel_main.o: Kernel/Main/main.c
	@echo "[CC] Building kernel..."
	$(CC) $(CCFLAGS) -mcmodel=kernel -fPIC -c $< -o $@

# Assembly - Context switching
context_switch.o: Kernel/Main/context_switch.asm
	@echo "[AS] Building context switch..."
	$(AS) $(ASFLAGS) $< -o $@

# Memory management
memory.o: Kernel/Memory/MemoryControl.c
	@echo "[CC] Building memory manager..."
	$(CC) $(CCFLAGS) -mcmodel=kernel -c $< -o $@

# IPC system
ipc.o: Kernel/Main/main.c
	@echo "[CC] Building IPC system..."
	$(CC) $(CCFLAGS) -mcmodel=kernel -DIPC_ONLY -c $< -o $@

# Link kernel ELF
kernel.elf: $(TARGETS)
	@echo "[LD] Linking kernel..."
	$(LD) $(LDFLAGS) -T linker.ld $(TARGETS) -o $@
	@echo "[INFO] Kernel image: $@"

# Create ISO image
kernel.iso: kernel.elf
	@echo "[BUILD] Creating ISO image..."
	@mkdir -p iso/EFI/BOOT
	@mkdir -p iso/Kernel/Main
	@cp bootloader.efi iso/EFI/BOOT/BOOTX64.EFI
	@cp kernel.elf iso/Kernel/Main/kernel.elf
	@mkisofs -R -J -b boot.img -no-emul-boot \
	         -boot-load-size 4 -input-charset utf-8 \
	         -o kernel.iso iso/

# Debugging info
objdump: kernel.elf
	@echo "[DEBUG] Disassembling kernel..."
	objdump -d kernel.elf | head -100

symbols: kernel.elf
	@echo "[DEBUG] Kernel symbols:"
	nm -n kernel.elf | tail -20

# Cleanup
clean:
	@echo "[CLEAN] Removing build artifacts..."
	rm -f *.o *.elf *.iso
	rm -rf iso/

mrclean: clean
	@echo "[MRCLEAN] Removing all generated files..."
	find . -name "*.o" -delete
	find . -name "*.a" -delete

# Help
help:
	@echo "Fexox OS x64 Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build kernel ELF and ISO image"
	@echo "  kernel.elf  - Build kernel ELF only"
	@echo "  kernel.iso  - Build ISO image only"
	@echo "  clean       - Remove build artifacts"
	@echo "  objdump     - Show disassembly"
	@echo "  symbols     - Show symbol table"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Compiler flags:"
	@echo "  -ffreestanding  : No linking against libc"
	@echo "  -mcmodel=kernel : 64-bit kernel code model"
	@echo "  -mno-red-zone   : No red zone (required for kernel)"
	@echo "  -mno-sse        : No SSE (for simplicity)"

.PHONY: all clean mrclean objdump symbols help
