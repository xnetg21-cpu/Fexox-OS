#include <efi.h>
#include <efilib.h>
#include <string.h>

// ===================================
// GDT DESCRIPTOR (64-bit x86-64)
// ===================================

typedef struct {
    uint16_t size;
    uint64_t base;
} __attribute__((packed)) GdtPointer;

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed)) GdtEntry32;

typedef struct {
    uint32_t zero1;
    uint64_t base;
    uint32_t zero2;
    uint32_t zero3;
} __attribute__((packed)) GdtEntry64;

// GDT таблица для x64
#define GDT_ENTRIES 6

GdtEntry32 gdt32[GDT_ENTRIES];
GdtEntry64 gdt64_tss;
uint8_t gdt_raw[GDT_ENTRIES * 8 + 16] __attribute__((aligned(16)));

// ===================================
// ELF64 структуры
// ===================================

#define PT_LOAD 1
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

#define KERNEL_PATH L"\\Kernel\\Main\\kernel.elf"
#define KERNEL_BASE 0xFFFFFFFF80000000  // Higher half kernel

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

// ===================================
// PAGE TABLE STRUCTURES (4-level paging)
// ===================================

#define PAGE_PRESENT   0x001
#define PAGE_WRITE     0x002
#define PAGE_USER      0x004
#define PAGE_PWT       0x008
#define PAGE_PCD       0x010
#define PAGE_ACCESSED  0x020
#define PAGE_DIRTY     0x040
#define PAGE_PAT       0x080
#define PAGE_GLOBAL    0x100
#define PAGE_NX        0x8000000000000000ULL  // No Execute

typedef uint64_t PageEntry;

PageEntry pml4[512] __attribute__((aligned(4096)));
PageEntry pml3_low[512] __attribute__((aligned(4096)));
PageEntry pml3_high[512] __attribute__((aligned(4096)));
PageEntry pml2_low[512] __attribute__((aligned(4096)));
PageEntry pml2_high[512] __attribute__((aligned(4096)));

// ===================================
// GDT INITIALIZATION
// ===================================

void setup_gdt() {
    // NULL descriptor
    gdt32[0].access = 0x00;
    
    // Code descriptor (64-bit)
    gdt32[1].limit_low = 0xFFFF;
    gdt32[1].base_low = 0;
    gdt32[1].base_mid = 0;
    gdt32[1].access = 0x9A;      // Present, Ring 0, Code segment, Readable
    gdt32[1].granularity = 0xA0; // 4KB granularity, 64-bit mode
    gdt32[1].base_high = 0;
    
    // Data descriptor (64-bit)
    gdt32[2].limit_low = 0xFFFF;
    gdt32[2].base_low = 0;
    gdt32[2].base_mid = 0;
    gdt32[2].access = 0x92;      // Present, Ring 0, Data segment, Writable
    gdt32[2].granularity = 0xA0; // 4KB granularity
    gdt32[2].base_high = 0;
    
    // User Code descriptor
    gdt32[3].limit_low = 0xFFFF;
    gdt32[3].base_low = 0;
    gdt32[3].base_mid = 0;
    gdt32[3].access = 0xFA;      // Present, Ring 3, Code
    gdt32[3].granularity = 0xA0;
    gdt32[3].base_high = 0;
    
    // User Data descriptor
    gdt32[4].limit_low = 0xFFFF;
    gdt32[4].base_low = 0;
    gdt32[4].base_mid = 0;
    gdt32[4].access = 0xF2;      // Present, Ring 3, Data
    gdt32[4].granularity = 0xA0;
    gdt32[4].base_high = 0;
    
    // TSS (не используется в bootloader, но обязателен)
    gdt32[5].limit_low = 0;
    gdt32[5].access = 0x89;
    gdt32[5].granularity = 0x00;
}

// ===================================
// PAGE TABLE SETUP (Identity + Higher half)
// ===================================

void setup_paging() {
    // Инициализируем таблицы нулями
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
        pml3_low[i] = 0;
        pml3_high[i] = 0;
        pml2_low[i] = 0;
        pml2_high[i] = 0;
    }
    
    // Identity mapping для нижней половины (0x0 - 0x40000000)
    pml4[0] = (PageEntry)pml3_low | PAGE_PRESENT | PAGE_WRITE;
    pml3_low[0] = (PageEntry)pml2_low | PAGE_PRESENT | PAGE_WRITE;
    
    for (int i = 0; i < 512; i++) {
        pml2_low[i] = ((uint64_t)i << 21) | PAGE_PRESENT | PAGE_WRITE | PAGE_NX;
    }
    
    // Higher half mapping (0xFFFF800000000000 - 0xFFFF800040000000)
    pml4[256] = (PageEntry)pml3_high | PAGE_PRESENT | PAGE_WRITE;
    pml3_high[0] = (PageEntry)pml2_high | PAGE_PRESENT | PAGE_WRITE;
    
    for (int i = 0; i < 512; i++) {
        pml2_high[i] = ((uint64_t)i << 21) | PAGE_PRESENT | PAGE_WRITE;
    }
}

// ===================================
// ЗАЩИТНЫЕ ФЛАГИ ПРОЦЕССОРА
// ===================================

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
    uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

void enable_cpu_protections() {
    uint64_t cr3, cr4;
    uint32_t eax, ebx, ecx, edx;
    int has_smep = 0;
    int has_smap = 0;

    // Загружаем CR3 с таблицей страниц
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)pml4));

    // Читаем CR4 и устанавливаем обязательные флаги
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    cr4 |= (1ULL << 5);  // PAE - Physical Address Extension
    cr4 |= (1ULL << 7);  // PGE - Page Global Enable

    // Проверяем поддержку SMEP/SMAP через CPUID. Если не поддерживается, оставляем флаги выключенными.
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        has_smep = (ebx & (1u << 7)) != 0;
        has_smap = (ebx & (1u << 8)) != 0;
    }

    if (has_smep) {
        cr4 |= (1ULL << 18);  // SMEP
    } else {
        cr4 &= ~(1ULL << 18);
    }

    if (has_smap) {
        cr4 |= (1ULL << 19);  // SMAP
    } else {
        cr4 &= ~(1ULL << 19);
    }

    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    // Читаем CR0 и включаем paging + NX
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));

    cr0 |= (1ULL << 31);  // PG - Paging enable
    cr0 |= (1ULL << 0);   // PE - Protection enable
    cr0 &= ~(1ULL << 2);  // EM - Floating point, disable
    cr0 |= (1ULL << 3);   // TS - Task switched (переключение контекста)

    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    // Включаем NX-бит в EFER
    __asm__ volatile ("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0xC0000080));
    eax |= (1u << 11);  // NX Enable
    eax |= (1u << 8);   // LME - Long Mode Enable
    __asm__ volatile ("wrmsr" : : "a"(eax), "d"(edx), "c"(0xC0000080));
}

// ===================================
// БЕЗОПАСНАЯ ЗАГРУЗКА ELF
// ===================================

EFI_STATUS safe_load_kernel_segment(
    Elf64_Phdr *phdr,
    uint8_t *kernel_buffer,
    uint64_t kernel_file_size) {
    
    // Проверки безопасности
    if (phdr->p_offset + phdr->p_filesz > kernel_file_size) {
        Print(L"ERROR: Segment offset exceeds file size\n");
        return EFI_LOAD_ERROR;
    }
    
    if (phdr->p_paddr < 0x1000) {
        Print(L"ERROR: Segment address too low (0x%x)\n", phdr->p_paddr);
        return EFI_LOAD_ERROR;
    }
    
    if (phdr->p_memsz == 0) {
        return EFI_SUCCESS;
    }
    
    uint8_t *segment_data = kernel_buffer + phdr->p_offset;
    uint8_t *target_addr = (uint8_t*)phdr->p_paddr;
    
    // Копируем данные с проверкой выравнивания
    if (phdr->p_filesz > 0) {
        memcpy(target_addr, segment_data, phdr->p_filesz);
    }
    
    // Очищаем BSS (неинициализированные данные)
    if (phdr->p_memsz > phdr->p_filesz) {
        memset(target_addr + phdr->p_filesz, 0, 
               phdr->p_memsz - phdr->p_filesz);
    }
    
    return EFI_SUCCESS;
}

EFI_STATUS load_kernel_from_file(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable,
    uint64_t *kernel_entry) {
    
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *kernel_file;
    
    Elf64_Ehdr *kernel_header;
    Elf64_Phdr *program_headers;
    UINTN kernel_size = 0;
    
    Print(L"[SECURE BOOTLOADER] Starting kernel load...\n");
    
    // Получаем доступ к образу
    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
        ImageHandle, &gEfiLoadedImageProtocolGuid, (void**)&loaded_image);
    if (EFI_ERROR(status)) {
        Print(L"[ERROR] Cannot get loaded image\n");
        return status;
    }
    
    // Получаем файловую систему
    status = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
        loaded_image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void**)&fs);
    if (EFI_ERROR(status)) {
        Print(L"[ERROR] Cannot get filesystem\n");
        return status;
    }
    
    // Открываем корневой каталог
    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(status)) {
        Print(L"[ERROR] Cannot open volume\n");
        return status;
    }
    
    // Открываем файл ядра
    status = uefi_call_wrapper(root->Open, 5, root, &kernel_file,
        KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"[ERROR] Kernel file not found\n");
        return status;
    }
    
    Print(L"[SECURE BOOTLOADER] Kernel file opened\n");
    
    // Получаем размер файла
    EFI_FILE_INFO *file_info;
    UINTN file_info_size = sizeof(EFI_FILE_INFO) + 256;
    
    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, file_info_size, (void**)&file_info);
    if (EFI_ERROR(status)) return status;
    
    status = uefi_call_wrapper(kernel_file->GetInfo, 4, kernel_file,
        &gEfiFileInfoGuid, &file_info_size, file_info);
    if (EFI_ERROR(status)) return status;
    
    kernel_size = file_info->FileSize;
    if (kernel_size > 16 * 1024 * 1024) {  // Max 16MB
        Print(L"[ERROR] Kernel too large: %d bytes\n", kernel_size);
        return EFI_LOAD_ERROR;
    }
    
    Print(L"[SECURE BOOTLOADER] Kernel size: %d bytes\n", kernel_size);
    
    // Выделяем память для ядра
    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, kernel_size, (void**)&kernel_header);
    if (EFI_ERROR(status)) {
        Print(L"[ERROR] Cannot allocate kernel memory\n");
        return status;
    }
    
    // Читаем ядро
    UINTN read_size = kernel_size;
    status = uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &read_size, kernel_header);
    if (EFI_ERROR(status)) {
        Print(L"[ERROR] Cannot read kernel file\n");
        return status;
    }
    
    // Проверяем ELF заголовок
    if (kernel_header->e_ident[0] != 0x7F ||
        kernel_header->e_ident[1] != 'E' ||
        kernel_header->e_ident[2] != 'L' ||
        kernel_header->e_ident[3] != 'F') {
        Print(L"[ERROR] Invalid ELF signature\n");
        return EFI_LOAD_ERROR;
    }
    
    if (kernel_header->e_machine != 0x3E) {  // EM_X86_64
        Print(L"[ERROR] Not x86-64 binary (machine=0x%x)\n", kernel_header->e_machine);
        return EFI_LOAD_ERROR;
    }
    
    if (kernel_header->e_phentsize < sizeof(Elf64_Phdr)) {
        Print(L"[ERROR] Invalid program header size\n");
        return EFI_LOAD_ERROR;
    }
    
    Print(L"[SECURE BOOTLOADER] ELF valid, entry=0x%x, PH count=%d\n", 
        kernel_header->e_entry, kernel_header->e_phnum);
    
    // Получаем программные заголовки
    program_headers = (Elf64_Phdr*)((uint8_t*)kernel_header + kernel_header->e_phoff);
    
    // Загружаем каждый сегмент
    for (int i = 0; i < kernel_header->e_phnum; i++) {
        Elf64_Phdr *phdr = &program_headers[i];
        
        if (phdr->p_type != PT_LOAD) continue;
        
        Print(L"[SECURE BOOTLOADER] Loading segment %d: paddr=0x%x size=0x%x\n",
            i, phdr->p_paddr, phdr->p_memsz);
        
        // Безопасно загружаем сегмент
        status = safe_load_kernel_segment(phdr, (uint8_t*)kernel_header, kernel_size);
        if (EFI_ERROR(status)) {
            return status;
        }
    }
    
    *kernel_entry = kernel_header->e_entry;
    
    uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
    uefi_call_wrapper(root->Close, 1, root);
    
    Print(L"[SECURE BOOTLOADER] Kernel loaded successfully at 0x%x\n", *kernel_entry);
    return EFI_SUCCESS;
}

// ===================================
// MAIN UEFI ENTRY POINT
// ===================================

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    Print(L"\n");
    Print(L"╔════════════════════════════════════════╗\n");
    Print(L"║  Fexox OS x64 Secure UEFI Bootloader  ║\n");
    Print(L"║  Protected Mode with NX/SMEP/SMAP     ║\n");
    Print(L"╚════════════════════════════════════════╝\n\n");

    uint64_t kernel_entry = 0;
    EFI_STATUS status;
    
    // Инициализируем GDT
    Print(L"[INIT] Setting up GDT...\n");
    setup_gdt();
    
    // Инициализируем таблицы страниц
    Print(L"[INIT] Setting up page tables (Identity + Higher half)...\n");
    setup_paging();
    
    // Загружаем ядро
    Print(L"[INIT] Loading kernel ELF...\n");
    status = load_kernel_from_file(ImageHandle, SystemTable, &kernel_entry);
    if (EFI_ERROR(status)) {
        Print(L"[CRITICAL] Kernel load failed with status 0x%x\n", status);
        return status;
    }
    
    // Выходим из boot services
    Print(L"[INIT] Exiting UEFI boot services...\n");
    
    UINTN map_key;
    UINTN desc_size;
    UINT32 desc_version;
    
    status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
        &map_key, NULL, &map_key, &desc_size, &desc_version);
    
    status = uefi_call_wrapper(SystemTable->BootServices->ExitBootServices, 2,
        ImageHandle, map_key);
    
    if (EFI_ERROR(status)) {
        Print(L"[WARNING] ExitBootServices failed, retrying...\n");
        // Try again with fresh memory map
        status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
            &map_key, NULL, &map_key, &desc_size, &desc_version);
        status = uefi_call_wrapper(SystemTable->BootServices->ExitBootServices, 2,
            ImageHandle, map_key);
    }
    
    // Включаем защитные механизмы процессора
    Print(L"[INIT] Enabling CPU protection mechanisms (NX, SMEP, SMAP)...\n");
    enable_cpu_protections();
    
    Print(L"[JUMP] Transferring control to kernel at 0x%x\n", kernel_entry);
    
    // Передаем управление ядру
    typedef void (*kernel_main_t)(void);
    kernel_main_t kernel_main = (kernel_main_t)kernel_entry;
    
    // Отключаем прерывания перед передачей управления
    __asm__ volatile ("cli");
    
    kernel_main();
    
    // Никогда не должны сюда вернуться
    while (1) {
        __asm__ volatile ("hlt");
    }
    
    return EFI_SUCCESS;
}
