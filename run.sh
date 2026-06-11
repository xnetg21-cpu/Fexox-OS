#!/bin/bash
echo "=== Запуск FEXOS в QEMU ==="

to_win() { cygpath -m "$1" 2>/dev/null || echo "$1"; }

ROOT="$(cd "$(dirname "$0")" && pwd)"
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
CODE="/c/Program Files/qemu/share/edk2-x86_64-code.fd"
VARS="/c/Program Files/qemu/share/edk2-i386-vars.fd"
ESP="$ROOT/build/esp.img"
DATA="$ROOT/build/data.img"

[ -f "$QEMU" ] || { echo "[!] Нет $QEMU"; exit 1; }
[ -f "$CODE" ] && [ -f "$VARS" ] || { echo "[!] OVMF не найден"; exit 1; }
[ -f "$ESP"  ] || { echo "[!] Нет $ESP  — сначала: sh Compile.sh"; exit 1; }
[ -f "$DATA" ] || { echo "[!] Нет $DATA — сначала: sh Compile.sh"; exit 1; }

mkdir -p /c/fos
cp -f "$VARS" /c/fos/OVMF_VARS.fd

exec "$(to_win "$QEMU")" \
  -machine q35 -m 256M -cpu qemu64 -vga std \
  \
  `# UEFI firmware` \
  -drive "if=pflash,format=raw,readonly=on,file=$(to_win "$CODE")" \
  -drive "if=pflash,format=raw,file=C:/fos/OVMF_VARS.fd" \
  \
  `# ESP: только для UEFI bootloader, НЕ virtio-blk` \
  -drive "if=none,id=esp,format=raw,file=$(to_win "$ESP"),readonly=on" \
  -device "virtio-scsi-pci,id=scsi0" \
  -device "scsi-cd,drive=esp,bus=scsi0.0" \
  \
  `# DATA: отдельный диск для ядра — это то что видит virtio-blk драйвер` \
  -drive "if=none,id=vda,format=raw,file=$(to_win "$DATA")" \
  -device "virtio-blk-pci,drive=vda,disable-modern=on,disable-legacy=off" \
  \
  -serial stdio \
  -no-reboot \
  -d int,cpu_reset,guest_errors,unimp,trace:virtio_blk_*,trace:virtio_queue_notify,trace:pci_* \
  -D qemu.log
