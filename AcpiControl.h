#ifndef ACPI_CONTROL_H
#define ACPI_CONTROL_H

#include <stdint.h>

/*
 * AcpiControl.h — минимальный ACPI-стек FEXOS: только то, что нужно для
 * НАСТОЯЩЕГО выключения (S5) и перезагрузки на реальном железе, а не
 * только в QEMU/Bochs.
 *
 * Что делает:
 *   1. Парсит RSDP -> RSDT/XSDT -> FADT (без полного AML-интерпретатора).
 *   2. Ищет в DSDT (и, если не нашлось, во всех SSDT) байтовый шаблон
 *      объекта \_S5 и вытаскивает из него SLP_TYPa/SLP_TYPb — значения,
 *      которые чипсет ожидает в PM1_CNT для перехода в S5 (soft-off).
 *      Это стандартный "no-AML-interpreter" приём (см. ACPI spec §16 —
 *      структура пакета \_S5 всегда одна и та же).
 *   3. Даёт acpi_power_off() — пишет SLP_TYP|SLP_EN в PM1a/PM1b_CNT_BLK.
 *   4. Даёт acpi_reset() — использует ACPI Reset Register (FADT, ACPI
 *      2.0+), если чипсет его поддерживает (RESET_REG_SUP) — это самый
 *      надёжный программный способ перезагрузки на реальном ПК.
 *
 * Вызывать acpi_init() ОДИН РАЗ, после mem_control_init (нужен активный
 * DIRECT_MAP — ACPI-таблицы читаются через phys_to_kvirt).
 *
 * Если ACPI недоступен/не распарсился — acpi_init() вернёт -1, а
 * acpi_power_off()/acpi_reset() будут no-op (вернут -1), вызывающий код
 * (см. Framebuffer.c: ui_system_shutdown/ui_system_reboot) должен сам
 * иметь фолбэки (QEMU debug-порт, 8042 reset, triple fault).
 */

/* rsdp_phys — физический адрес RSDP, полученный от загрузчика через
 * BOOT_INFO.rsdp_phys (EFI ConfigurationTable, GUID ACPI 2.0/1.0).
 * Возвращает 0 при успехе, -1 если ACPI не найден/не распарсен. */
int acpi_init(uint64_t rsdp_phys);

/* Настоящее выключение через ACPI (SLP_TYP|SLP_EN в PM1_CNT).
 * При успехе НЕ ВОЗВРАЩАЕТСЯ (питание отключается физически).
 * Возвращает -1, если ACPI не готов / \_S5 не найден — тогда
 * вызывающий код должен использовать свой фолбэк. */
int acpi_power_off(void);

/* Настоящая перезагрузка через ACPI Reset Register (FADT, ACPI 2.0+).
 * При успехе НЕ ВОЗВРАЩАЕТСЯ. Возвращает -1, если чипсет не
 * поддерживает Reset Register (нет RESET_REG_SUP) — тогда вызывающий
 * код должен использовать свой фолбэк (8042, triple fault). */
int acpi_reset(void);

/* Абсолютный последний фолбэк перезагрузки — triple fault (lidt с
 * limit=0 + программное исключение). Работает на любом x86, включая
 * самое старое реальное железо без ACPI и без рабочего 8042.
 * НИКОГДА не возвращается. */
void cpu_triple_fault_reset(void);

#endif /* ACPI_CONTROL_H */
