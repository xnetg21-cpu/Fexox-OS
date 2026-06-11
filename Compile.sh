#!/bin/bash
set -e
echo "=== FEXOS Build Script (MSYS2 UCRT64) ==="

CC="clang"
LD_KERN="ld.lld"

command -v $CC >/dev/null || { echo "[!] clang не найден"; exit 1; }

mkdir -p build

CFLAGS_KERN="-I. -target x86_64-unknown-none-elf -ffreestanding -fno-builtin -mno-red-zone -mcmodel=kernel -fno-pie -fno-stack-protector -mgeneral-regs-only -O0 -g -Wall -Wextra -std=c11"
LDFLAGS_KERN="-m elf_x86_64 -nostdlib -T kernel.ld"

MGW="x86_64-w64-mingw32-gcc"
command -v $MGW >/dev/null || { echo "[!] $MGW не найден"; exit 1; }

CFLAGS_BOOT="-I. -ffreestanding -fno-builtin -mno-red-zone -fno-stack-check -fno-stack-protector -mno-stack-arg-probe -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -O0 -g -std=c11"
LDFLAGS_BOOT="-nostdlib -nodefaultlibs -Wl,--subsystem,10 -Wl,--image-base=0x10000000 -Wl,--gc-sections -Wl,--emit-relocs -e efi_main"

echo "[*] Ядро..."
$CC $CFLAGS_KERN -c boot_stub.c -o build/boot_stub.o
$CC $CFLAGS_KERN -c main.c -o build/main.o
$CC $CFLAGS_KERN -c MemoryControl.c -o build/MemoryControl.o
$CC $CFLAGS_KERN -c InterruptControl.c -o build/InterruptControl.o
$CC $CFLAGS_KERN -c Scheduler.c -o build/Scheduler.o
$CC $CFLAGS_KERN -c VFS.c      -o build/VFS.o
$CC $CFLAGS_KERN -c FAT32.c    -o build/FAT32.o
$CC $CFLAGS_KERN -c klibc.c    -o build/klibc.o
$LD_KERN $LDFLAGS_KERN -o build/kernel.bin build/boot_stub.o build/main.o build/MemoryControl.o build/InterruptControl.o build/Scheduler.o build/VFS.o build/FAT32.o build/klibc.o

echo "[*] Загрузчик UEFI..."
$MGW $CFLAGS_BOOT -c bootloader.c -o build/bootloader.o
$MGW $LDFLAGS_BOOT build/bootloader.o -o build/bootloader.efi

ls -lh build/kernel.bin build/bootloader.efi

TARGET_DIR="/c/fos"
mkdir -p "$TARGET_DIR/EFI/BOOT"
cp -f build/kernel.bin "$TARGET_DIR/kernel.bin"
cp -f build/bootloader.efi "$TARGET_DIR/EFI/BOOT/BOOTX64.EFI"
rm -f "$TARGET_DIR/startup.nsh" "$TARGET_DIR/OVMF.fd" "$TARGET_DIR/OVMF_VARS.fd" 2>/dev/null || true

# Диск для QEMU: чистый FAT без MBR legacy-boot
if command -v mformat >/dev/null; then
    echo "[*] Диск build/esp.img..."
    dd if=/dev/zero of=build/esp.img bs=1M count=64 status=none
    mformat -i build/esp.img -F -v FEXOS ::
    mmd -i build/esp.img ::EFI ::EFI/BOOT
    
    mcopy -i build/esp.img build/bootloader.efi ::EFI/BOOT/BOOTX64.EFI
    mcopy -i build/esp.img build/kernel.bin ::kernel.bin
    
    # Создаем startup.nsh и добавляем его в корень образа esp.img
    echo "fs0:" > build/startup.nsh
    echo "\\EFI\\BOOT\\BOOTX64.EFI" >> build/startup.nsh
    mcopy -i build/esp.img build/startup.nsh ::startup.nsh
    echo "[OK] startup.nsh успешно добавлен в образ esp.img"

    # data.img — отдельный чистый FAT32 диск для virtio-blk.
    # Ядро монтирует именно его; esp.img используется только для загрузки UEFI.
    echo "[*] Диск build/data.img (virtio-blk data disk)..."
    dd if=/dev/zero of=build/data.img bs=1M count=64 status=none
    mformat -i build/data.img -F -v FEXDATA ::
    echo "[OK] build/data.img готов"
else
    echo "[!] mtools нет — pacman -S mingw-w64-ucrt-x86_64-mtools"
fi

echo "[+] build/kernel.bin  build/bootloader.efi  build/esp.img  build/data.img"
echo "=== Готово ==="
