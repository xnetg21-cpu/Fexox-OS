/* Linker script для Fexox OS x64 Kernel */

/* Memory layout */
MEMORY
{
    /* Identity-mapped region (0x0) */
    boot : ORIGIN = 0x00100000, LENGTH = 4M
    
    /* Higher-half kernel (canonical form) */
    kernel : ORIGIN = 0xFFFFFFFF80000000, LENGTH = 1G
}

SECTIONS
{
    /* Bootloader и early init code */
    .boot : {
        KEEP(*(.multiboot))
        *(.boot)
        *(.boot.text)
    } > boot
    
    /* Kernel текст в higher-half */
    .text : ALIGN(4096) {
        *(.text)
        *(.text.*)
        . = ALIGN(4096);
    } > kernel AT > boot
    
    /* Read-only data */
    .rodata : ALIGN(4096) {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4096);
    } > kernel AT > boot
    
    /* Data */
    .data : ALIGN(4096) {
        *(.data)
        *(.data.*)
        . = ALIGN(4096);
    } > kernel AT > boot
    
    /* BSS (initialized to zero) */
    .bss : ALIGN(4096) {
        *(COMMON)
        *(.bss)
        *(.bss.*)
        . = ALIGN(4096);
    } > kernel AT > boot
    
    /* Garbage collection of unused sections */
    /DISCARD/ : {
        *(.note)
        *(.note.*)
        *(.gnu.version)
        *(.gnu.version.*)
    }
}

/* Export kernel symbols */
kernel_start = ADDR(.text);
kernel_end = ADDR(.bss) + SIZEOF(.bss);
kernel_size = kernel_end - kernel_start;
