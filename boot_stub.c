/* boot_stub.c — точка входа ядра (higher-half) */
#include <stdint.h>
#include "debug_out.h"
#include "Framebuffer.h"

typedef struct __attribute__((packed)) {
    uint64_t magic;
    void    *mem_map;
    uint64_t mem_map_size;
    uint64_t desc_size;
    uint32_t desc_version;
    uint64_t kernel_entry;
    uint64_t pml4_phys;
    void    *gdt_ptr;
    void    *idt_ptr;
    fb_info_t fb;
} BOOT_INFO;

extern void kernel_entry(BOOT_INFO *info);
extern char kernel_stack_top[];

__attribute__((section(".boot")))
void boot_stub_entry(BOOT_INFO *info) {
    DBG_MSG("KR", "00 boot_stub_entry");

    if (!info)
        DBG_PANIC("KR", "BOOT_INFO is NULL");

    DBG_VAL("KR", "info", (uint64_t)(uintptr_t)info);
    DBG_VAL("KR", "magic", info->magic);
    DBG_VAL("KR", "pml4", info->pml4_phys);
    DBG_VAL("KR", "entry", info->kernel_entry);

    if (info->magic != 0x4B45524E454C424FULL)
        DBG_PANIC("KR", "bad BOOT_INFO magic");

    DBG_MSG("KR", "01 set stack");
    __asm__ volatile (
        "mov %0, %%rsp"
        :
        : "r" ((uint64_t)(uintptr_t)&kernel_stack_top)
        : "memory"
    );
    DBG_VAL("KR", "rsp", (uint64_t)(uintptr_t)&kernel_stack_top);

    DBG_MSG("KR", "02 call kernel_entry");
    kernel_entry(info);

    DBG_PANIC("KR", "kernel_entry returned");
}
