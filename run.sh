#!/bin/bash
echo "=== Запуск FEXOS в QEMU ==="

to_win() { cygpath -m "$1" 2>/dev/null || echo "$1"; }

ROOT="$(cd "$(dirname "$0")" && pwd)"
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
CODE="/c/Program Files/qemu/share/edk2-x86_64-code.fd"
VARS="/c/Program Files/qemu/share/edk2-i386-vars.fd"
ESP="$ROOT/build/esp.img"

[ -f "$QEMU" ] || { echo "[!] Нет $QEMU"; exit 1; }
[ -f "$CODE" ] && [ -f "$VARS" ] || { echo "[!] OVMF в QEMU: code.fd + edk2-i386-vars.fd"; exit 1; }
[ -f "$ESP" ] || { echo "[!] Нет $ESP — сначала: sh Compile.sh"; exit 1; }

mkdir -p /c/fos
cp -f "$VARS" /c/fos/OVMF_VARS.fd

exec "$(to_win "$QEMU")" \
  -machine q35 -m 256M -cpu qemu64 -vga std \
  -drive "if=pflash,format=raw,readonly=on,file=$(to_win "$CODE")" \
  -drive "if=pflash,format=raw,file=C:/fos/OVMF_VARS.fd" \
  -drive "if=none,id=esp,format=raw,file=$(to_win "$ESP")" \
  -device virtio-blk-pci,drive=esp,bootindex=1 \
  -serial stdio \
  -no-reboot \
  -d int,cpu_reset,guest_errors,unimp \
  -D qemu.log