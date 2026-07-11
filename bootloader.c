/* bootloader.c — UEFI загрузчик FEXOS + пошаговая отладка (serial + экран) */
#include <stdint.h>
#include "debug_out.h"

#define EFIAPI __attribute__((ms_abi))
#define PACKED __attribute__((packed, aligned(1)))
#define NULL ((void *)0)
#define PAGE_SIZE 4096

typedef uint64_t  UINT64;
typedef uint32_t  UINT32;
typedef uint16_t  UINT16;
typedef uint8_t   UINT8;
typedef UINT8     BOOLEAN;
typedef UINT64    UINTN;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINTN     EFI_STATUS;

#define EFI_SUCCESS           ((EFI_STATUS)0)
#define EFI_BUFFER_TOO_SMALL  ((EFI_STATUS)(5  | (1ULL << 63)))
#define EFI_UNSUPPORTED       ((EFI_STATUS)(3  | (1ULL << 63)))
#define EFI_NOT_FOUND         ((EFI_STATUS)(14 | (1ULL << 63)))
#define EFI_LOAD_ERROR        ((EFI_STATUS)(2  | (1ULL << 63)))
#define EFI_INVALID_PARAMETER ((EFI_STATUS)(5  | (1ULL << 63)))

typedef void *EFI_HANDLE;

typedef enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData,
    EfiBootServicesCode, EfiBootServicesData, EfiRuntimeServicesCode,
    EfiRuntimeServicesData, EfiConventionalMemory, EfiUnusableMemory,
    EfiACPIReclaimMemory, EfiACPIMemoryNVS, EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace, EfiPalCode, EfiPersistentMemory,
    EfiUnacceptedMemoryType, EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress, MaxAllocateType } EFI_ALLOCATE_TYPE;
typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;

typedef struct PACKED {
    UINT32              Type;
    UINT32              Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    UINT64              VirtualStart;
    UINT64              NumberOfPages;
    UINT64              Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8  Data4[8];
} EFI_GUID;

static EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID =
    {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static EFI_GUID EFI_FILE_INFO_ID =
    {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID =
    {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID =
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

/* ACPI RSDP GUID'ы в EFI ConfigurationTable — нужны для поиска RSDP без
 * сканирования памяти (на реальном железе RSDP не всегда лежит в
 * "классическом" диапазоне 0xE0000-0xFFFFF, как в BIOS/QEMU). */
static EFI_GUID EFI_ACPI_20_TABLE_GUID =
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static EFI_GUID EFI_ACPI_10_TABLE_GUID =
    {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

static int efi_guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) return 0;
    for (int i = 0; i < 8; i++) if (a->Data4[i] != b->Data4[i]) return 0;
    return 1;
}

/* ---- GOP (Graphics Output Protocol) ---- */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                             MaxMode;
    UINT32                             Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                              SizeOfInfo;
    EFI_PHYSICAL_ADDRESS               FrameBufferBase;
    UINTN                              FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- fb_info_t (должна совпадать с Framebuffer.h) ---- */
#define FB_MODE_NONE    0
#define FB_MODE_VGA     1
#define FB_MODE_LINEAR  2

typedef struct {
    UINT8  mode;
    UINT32 width;
    UINT32 height;
    UINT32 pitch;
    UINT8  bpp;
    UINT64 phys_addr;
    UINT32 red_mask;
    UINT32 green_mask;
    UINT32 blue_mask;
} fb_info_t;

typedef struct {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Reset)(void *, UINT8);
    EFI_STATUS (EFIAPI *OutputString)(void *, UINT16 *);
    EFI_STATUS (EFIAPI *TestString)(void *, UINT16 *);
    EFI_STATUS (EFIAPI *QueryMode)(void *, UINTN, UINTN *, UINTN *);
    EFI_STATUS (EFIAPI *SetMode)(void *, UINTN);
    EFI_STATUS (EFIAPI *SetAttribute)(void *, UINTN);
    EFI_STATUS (EFIAPI *ClearScreen)(void *);
    EFI_STATUS (EFIAPI *SetCursorPosition)(void *, UINTN, UINTN);
    EFI_STATUS (EFIAPI *EnableCursor)(void *, BOOLEAN);
    void *CurrentMode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct PACKED {
    UINT64 Size; UINT64 FileSize; UINT64 PhysicalSize;
    UINT64 CreateTime; UINT64 LastAccessTime; UINT64 ModificationTime;
    UINT64 Attribute; UINT16 FileName[256];
} EFI_FILE_INFO;

typedef struct {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *, void **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(void *, void **, UINT16 *, UINT64, UINT64);
    EFI_STATUS (EFIAPI *Close)(void *);
    EFI_STATUS (EFIAPI *Delete)(void *);
    EFI_STATUS (EFIAPI *Read)(void *, UINTN *, void *);
    EFI_STATUS (EFIAPI *Write)(void *, UINTN *, void *);
    EFI_STATUS (EFIAPI *GetPosition)(void *, UINT64 *);
    EFI_STATUS (EFIAPI *SetPosition)(void *, UINT64);
    EFI_STATUS (EFIAPI *GetInfo)(void *, EFI_GUID *, UINTN *, void *);
    EFI_STATUS (EFIAPI *SetInfo)(void *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (EFIAPI *Flush)(void *);
} EFI_FILE_PROTOCOL;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    UINT32 Revision;
    UINT32 Pad;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *ImageBase;
    UINTN ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE);
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL; void *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, UINT32 *);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    void *CreateEvent; void *SetTimer; void *WaitForEvent; void *SignalEvent; void *CloseEvent; void *CheckEvent;
    void *InstallProtocolInterface; void *ReinstallProtocolInterface; void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved; void *RegisterProtocolNotify;
    EFI_STATUS (EFIAPI *LocateHandle)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, void *, UINTN *, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *LocateDevicePath)(EFI_GUID *, void **, void **);
    EFI_STATUS (EFIAPI *InstallConfigurationTable)(EFI_GUID *, void *);
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE, void *, void *, UINTN, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, UINT16 **);
    void *Exit;
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE);
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
    void *GetNextMonotonicCount; void *Stall;
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, UINT64, UINTN, UINT16 *);
    void *ConnectController; void *DisconnectController; void *OpenProtocol; void *CloseProtocol; void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, void *, UINTN *, EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
    void *InstallMultipleProtocolInterfaces; void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32; void *CopyMem; void *SetMem; void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    UINT64 Signature; UINT32 Revision; UINT32 HeaderSize; UINT32 CRC32; UINT32 Reserved;
    EFI_HANDLE FirmWareVendor; UINT32 FirmWareRevision;
    EFI_HANDLE ConsoleInHandle; void *ConIn;
    EFI_HANDLE ConsoleOutHandle; EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle; void *StdErr;
    void *RuntimeServices; EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries; void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* Ищем RSDP в EFI ConfigurationTable: сперва ACPI 2.0, затем ACPI 1.0 как
 * фолбэк. Возвращает физический адрес RSDP или 0, если ACPI не найден
 * (тогда ядро просто не сможет сделать "настоящий" ACPI shutdown/reboot
 * и останется на старых фолбэках — не фатально).
 * Определена здесь (а не сразу после GUID'ов), т.к. нужен уже готовый
 * тип EFI_SYSTEM_TABLE. */
static UINT64 efi_find_rsdp(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_CONFIGURATION_TABLE *cfg = (EFI_CONFIGURATION_TABLE *)SystemTable->ConfigurationTable;
    void *rsdp10 = NULL;
    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        if (efi_guid_eq(&cfg[i].VendorGuid, &EFI_ACPI_20_TABLE_GUID))
            return (UINT64)(UINTN)cfg[i].VendorTable;
        if (efi_guid_eq(&cfg[i].VendorGuid, &EFI_ACPI_10_TABLE_GUID))
            rsdp10 = cfg[i].VendorTable;
    }
    return (UINT64)(UINTN)rsdp10;
}

typedef struct PACKED {
    UINT8  e_ident[16]; UINT16 e_type; UINT16 e_machine; UINT32 e_version;
    UINT64 e_entry; UINT64 e_phoff; UINT64 e_shoff; UINT32 e_flags;
    UINT16 e_ehsize; UINT16 e_phentsize; UINT16 e_phnum;
    UINT16 e_shentsize; UINT16 e_shnum; UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct PACKED {
    UINT32 p_type; UINT32 p_flags;
    UINT64 p_offset; UINT64 p_vaddr; UINT64 p_paddr;
    UINT64 p_filesz; UINT64 p_memsz; UINT64 p_align;
} Elf64_Phdr;

#define PT_LOAD           1
#define KERNEL_VIRT_BASE  0xFFFFFFFF80000000ULL
#define KERNEL_PHYS_BASE  0x00200000ULL  /* MUST be 2MiB-aligned for 2MiB pages */

typedef struct PACKED {
    UINT64 magic;
    void    *mem_map;
    UINTN   mem_map_size;
    UINTN   desc_size;
    UINT32  desc_version;
    UINT64  kernel_entry;
    UINT64  pml4_phys;
    void    *gdt_ptr;
    void    *idt_ptr;
    fb_info_t fb;
    UINT64  rsdp_phys;   /* физический адрес ACPI RSDP, 0 если не найден */
} BOOT_INFO;

#define BOOT_INFO_MAGIC 0x4B45524E454C424FULL
#define BOOT_FILE_PHYS  0x00800000ULL  /* temp ELF buffer: 8 MiB, well above kernel at 2 MiB */
#define IDENTITY_GB     4ULL
#define PTE_P  (1ULL << 0)
#define PTE_W  (1ULL << 1)
#define PTE_PS (1ULL << 7)

static BOOT_INFO g_boot_info;
static UINT8 bl_exit_stack[32768] __attribute__((aligned(16)));

static void efi_memcpy(void *dst, const void *src, UINTN n) {
    UINT8 *d = dst;
    const UINT8 *s = src;
    while (n--) *d++ = *s++;
}

static void efi_memset(void *dst, UINT8 c, UINTN n) {
    UINT8 *d = dst;
    while (n--) *d++ = c;
}

static void efi_print(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut, const char *str) {
    if (!ConOut) return;
    static UINT16 buf[256];
    UINTN i = 0;
    while (str[i] && i < 255) {
        buf[i] = (UINT16)(UINT8)str[i];
        i++;
    }
    buf[i] = 0;
    ConOut->OutputString(ConOut, buf);
}

static void bl_log(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut, const char *str) {
    dbg_puts(str);
    efi_print(ConOut, str);
}

// ИСПРАВЛЕНО: ConOut передается по указателю, а не по значению
static void bl_fail(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut, const char *stage, EFI_STATUS st) {
    dbg_puts("\n*** BOOT FAILED at ");
    dbg_puts(stage);
    dbg_puts(" EFI status=");
    dbg_hex64((UINT64)st);
    dbg_puts(" ***\n");
    DBG_PANIC("BL", stage);
}

static UINT64 setup_boot_paging(UINT64 pml4_phys) {
    UINT64 *pml4    = (UINT64 *)(UINTN)pml4_phys;
    UINT64 *pdpt_id = (UINT64 *)(UINTN)(pml4_phys + 1 * PAGE_SIZE);
    UINT64 *pd_id0  = (UINT64 *)(UINTN)(pml4_phys + 2 * PAGE_SIZE);
    UINT64 *pd_id1  = (UINT64 *)(UINTN)(pml4_phys + 3 * PAGE_SIZE);
    UINT64 *pd_id2  = (UINT64 *)(UINTN)(pml4_phys + 4 * PAGE_SIZE);
    UINT64 *pd_id3  = (UINT64 *)(UINTN)(pml4_phys + 5 * PAGE_SIZE);
    UINT64 *pdpt_hi = (UINT64 *)(UINTN)(pml4_phys + 6 * PAGE_SIZE);
    UINT64 *pd_hi   = (UINT64 *)(UINTN)(pml4_phys + 7 * PAGE_SIZE);

    efi_memset(pml4, 0, 8 * PAGE_SIZE);

    // === Identity: 4 GiB через 2-MiB страницы ===
    pml4[0] = (UINT64)(UINTN)pdpt_id | PTE_P | PTE_W;
    
    // Один PDPT может указывать на 4 разные PD (каждая по 1 GiB)
    pdpt_id[0] = (UINT64)(UINTN)pd_id0 | PTE_P | PTE_W;
    pdpt_id[1] = (UINT64)(UINTN)pd_id1 | PTE_P | PTE_W;
    pdpt_id[2] = (UINT64)(UINTN)pd_id2 | PTE_P | PTE_W;
    pdpt_id[3] = (UINT64)(UINTN)pd_id3 | PTE_P | PTE_W;

    // Заполняем 4 таблицы (2048 записей * 2 MiB = 4 GiB)
    for (UINT64 i = 0; i < 2048; i++) {
        UINT64 *pd = (UINT64 *)(UINTN)(pml4_phys + (2 + (i / 512)) * PAGE_SIZE);
        pd[i % 512] = (i << 21) | PTE_P | PTE_W | PTE_PS; // << 21 сдвиг для 2 MiB
    }

    // === Higher-half: ядро через 2-MiB страницы ===
    UINT64 vaddr = KERNEL_VIRT_BASE;
    UINT64 pml4_idx = (vaddr >> 39) & 0x1FF;
    UINT64 pdpt_idx = (vaddr >> 30) & 0x1FF;
    UINT64 pd_idx   = (vaddr >> 21) & 0x1FF;

    pml4[pml4_idx] = (UINT64)(UINTN)pdpt_hi | PTE_P | PTE_W;
    pdpt_hi[pdpt_idx] = (UINT64)(UINTN)pd_hi | PTE_P | PTE_W;

    // Мапим 8 MiB (4 страницы по 2 MiB)
    for (UINT64 i = 0; i < 4; i++) {
        pd_hi[pd_idx + i] = (KERNEL_PHYS_BASE + (i << 21)) | PTE_P | PTE_W | PTE_PS;
    }

    return pml4_phys;
}

static void jump_to_kernel_virt(UINT64 pml4_phys, UINT64 entry, BOOT_INFO *info, UINT64 rsp) {
    DBG_MSG("BL", "20 jump: load CR3 + far jump entry");
    DBG_VAL("BL", "cr3", pml4_phys);
    DBG_VAL("BL", "entry", entry);
    DBG_VAL("BL", "rsp", rsp);
    DBG_VAL("BL", "info", (UINT64)(UINTN)info);

    // Минимальный GDT: Null, 64-bit Code (L=1), 64-bit Data
    static __attribute__((aligned(16))) UINT64 gdt[3] = {0};
    static struct { UINT16 limit; UINT64 base; } __attribute__((packed)) gdtr;

    // 0x00209A0000000000 -> P=1, DPL=0, S=1, Exec/Read, L=1 (64-bit mode)
    gdt[1] = 0x00209A0000000000ULL;
    // 0x0000920000000000 -> P=1, DPL=0, S=1, Data/RW
    gdt[2] = 0x0000920000000000ULL;
    
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (UINT64)(UINTN)gdt;

    __asm__ volatile (
        "cli\n\t"
        "mov %[cr3], %%cr3\n\t"   // Включаем пейджинг
        "lgdt %[gdtr]\n\t"        // Загружаем наш GDT
        
        // 🔥 FAR JUMP для перезагрузки CS (обязательно для long mode)
        "pushq $0x08\n\t"         // Селектор кодового сегмента (0x08 -> gdt[1])
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"               // Извлекает RIP и CS, переключая в 64-bit режим
        "1:\n\t"
        
        // Сбрасываем сегментные регистры данных на наш новый GDT
        "mov $0x10, %%ax\n\t"     // Селектор данных (0x10 -> gdt[2])
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        
        // Устанавливаем стек и передаём управление ядру
        "mov %[rsp], %%rsp\n\t"
        "mov %[arg], %%rdi\n\t"
        "jmp *%[entry]\n\t"
        :
        : [cr3]   "r" (pml4_phys),
          [gdtr]  "m" (gdtr),
          [rsp]   "r" (rsp),
          [arg]   "r" (info),
          [entry] "r" (entry)
        : "memory", "rax"
    );
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    DBG_MSG("BL", "00 efi_main START");
    if (!SystemTable || !SystemTable->BootServices) {
        DBG_PANIC("BL", "SystemTable invalid");
    }

    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut = SystemTable->ConOut;

    BS->SetWatchdogTimer(0, 0, 0, NULL);
    bl_log(ConOut, "=== FEXOS Bootloader ===\r\n");
    DBG_MSG("BL", "01 ConOut ok");

    /* RSDP берём из ConfigurationTable сразу, пока SystemTable точно валиден
     * (это просто чтение массива, не boot-service вызов — безопасно делать
     * и после ExitBootServices, но удобнее сохранить пораньше). */
    UINT64 rsdp_phys = efi_find_rsdp(SystemTable);
    DBG_VAL("BL", "01b rsdp_phys", rsdp_phys);

    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MemoryMapSize = 0, MapKey = 0, DescriptorSize = 0;
    UINT32 DescriptorVersion = 0;
    EFI_STATUS status;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL; // ИСПРАВЛЕНО: убран пробел в имени типа
    EFI_FILE_PROTOCOL *root = NULL;

    DBG_MSG("BL", "02 open FS (boot device)...");
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    status = BS->HandleProtocol(ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
    if (status == EFI_SUCCESS && loaded && loaded->DeviceHandle) {
        status = BS->HandleProtocol(loaded->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&fs);
        if (status == EFI_SUCCESS && fs && fs->OpenVolume(fs, (void **)&root) == EFI_SUCCESS)
            DBG_MSG("BL", "03 FS via LoadedImage");
    }

    if (!root) {
        EFI_HANDLE *handles = NULL;
        UINTN handle_count = 0;
        DBG_MSG("BL", "03 LocateHandleBuffer FS...");
        status = BS->LocateHandleBuffer(ByProtocol, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, NULL, &handle_count, &handles);
        if (status != EFI_SUCCESS)
            bl_fail(ConOut, "LocateHandleBuffer FS", status);

        for (UINTN i = 0; i < handle_count; i++) {
            fs = NULL;
            status = BS->HandleProtocol(handles[i], &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&fs);
            if (status != EFI_SUCCESS || !fs) continue;
            if (fs->OpenVolume(fs, (void **)&root) == EFI_SUCCESS) break;
            root = NULL;
        }
        BS->FreePool(handles);
    }

    if (!root) bl_fail(ConOut, "OpenVolume root", EFI_NOT_FOUND);
    DBG_MSG("BL", "04 FS ready");
    bl_log(ConOut, "[OK] FS mounted\r\n");
    DBG_MSG("BL", "05 root volume");

    UINT16 kernel_path[] = L"\\kernel.bin";
    EFI_FILE_PROTOCOL *kernel_file = NULL;
    status = root->Open(root, (void **)&kernel_file, kernel_path, 0x01, 0);
    if (status != EFI_SUCCESS) bl_fail(ConOut, "Open kernel.bin", status);
    DBG_MSG("BL", "06 kernel.bin opened");

    EFI_FILE_INFO *info_buf = NULL;
    UINTN info_size = sizeof(EFI_FILE_INFO) + (256 * sizeof(UINT16));
    BS->AllocatePool(EfiLoaderData, info_size, (void **)&info_buf);
    status = kernel_file->GetInfo(kernel_file, &EFI_FILE_INFO_ID, &info_size, info_buf);
    if (status != EFI_SUCCESS) bl_fail(ConOut, "GetInfo kernel", status);
    UINT64 file_size = info_buf->FileSize;
    BS->FreePool(info_buf);
    DBG_VAL("BL", "kernel bytes", file_size);

    EFI_PHYSICAL_ADDRESS file_buf = 0;
    UINTN file_pages = (UINTN)((file_size + PAGE_SIZE - 1) / PAGE_SIZE);
    status = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, file_pages, &file_buf);
    if (status != EFI_SUCCESS) bl_fail(ConOut, "Alloc file buffer", status);

    UINTN read_size = (UINTN)file_size;
    status = kernel_file->Read(kernel_file, &read_size, (void *)(UINTN)file_buf);
    kernel_file->Close(kernel_file);
    if (status != EFI_SUCCESS) bl_fail(ConOut, "Read kernel.bin", status);
    DBG_MSG("BL", "07 kernel read");

    Elf64_Ehdr *elf = (Elf64_Ehdr *)(UINTN)file_buf;
    if (elf->e_ident[0] != 0x7F || elf->e_ident[1] != 'E' || elf->e_ident[4] != 2)
        bl_fail(ConOut, "ELF64 check", EFI_UNSUPPORTED);

    DBG_VAL("BL", "e_entry", elf->e_entry);
    DBG_VAL("BL", "phnum", elf->e_phnum);

    Elf64_Phdr *phdr = (Elf64_Phdr *)((UINT8 *)elf + elf->e_phoff);
    UINTN seg_n = 0;
    for (UINT16 i = 0; i < elf->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        seg_n++;
        DBG_VAL("BL", "seg p_paddr", phdr[i].p_paddr);
        DBG_VAL("BL", "seg filesz", phdr[i].p_filesz);

        UINT64 seg_lo = phdr[i].p_paddr & ~((UINT64)PAGE_SIZE - 1);
        UINT64 seg_hi = phdr[i].p_paddr + phdr[i].p_memsz;
        UINT64 seg_end = (seg_hi + PAGE_SIZE - 1) & ~((UINT64)PAGE_SIZE - 1);
        UINTN seg_pages = (UINTN)((seg_end - seg_lo) / PAGE_SIZE);
        if (seg_pages == 0) continue;

        EFI_PHYSICAL_ADDRESS seg = seg_lo;
        status = BS->AllocatePages(AllocateAddress, EfiLoaderCode, seg_pages, &seg); // ИСПРАВЛЕНО: sta tus -> status
        if (status != EFI_SUCCESS) {
            dbg_puts("[BL] seg alloc failed i=");
            dbg_dec64(i);
            dbg_puts("\n");
            bl_fail(ConOut, "Alloc PT_LOAD seg", status);
        }

        if (phdr[i].p_filesz)
            efi_memcpy((void *)(UINTN)phdr[i].p_paddr, (UINT8 *)elf + phdr[i].p_offset, (UINTN)phdr[i].p_filesz);
        if (phdr[i].p_memsz > phdr[i].p_filesz)
            efi_memset((UINT8 *)(UINTN)phdr[i].p_paddr + phdr[i].p_filesz, 0, (UINTN)(phdr[i].p_memsz - phdr[i].p_filesz));
    }
    bl_log(ConOut, "[OK] ELF segments loaded\r\n");
    DBG_VAL("BL", "segments", seg_n);

    EFI_PHYSICAL_ADDRESS pt_phys = 0;
    // Выделяем 8 страниц: 1 PML4 + 1 PDPT (ID) + 4 PD (ID) + 1 PDPT (HI) + 1 PD (HI)
    status = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, 8, &pt_phys);
    if (status != EFI_SUCCESS) bl_fail(ConOut, "Alloc page tables", status);
    
    UINT64 pml4_phys = setup_boot_paging((UINT64)pt_phys);
    DBG_VAL("BL", "pml4", pml4_phys);
    DBG_MSG("BL", "08 paging built");

    /* ==========================================================
     * GOP — получаем framebuffer ДО ExitBootServices.
     * После EBS обращаться к Boot Services нельзя.
     * ========================================================== */
    DBG_MSG("BL", "08b GOP init...");
    efi_memset(&g_boot_info.fb, 0, sizeof(fb_info_t));
    g_boot_info.fb.mode = FB_MODE_VGA; /* fallback */
    {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        EFI_STATUS gop_status = BS->LocateProtocol(
            &EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, NULL, (void **)&gop);

        if (gop_status == EFI_SUCCESS && gop && gop->Mode && gop->Mode->Info) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;

            g_boot_info.fb.mode     = FB_MODE_LINEAR;
            g_boot_info.fb.width    = mi->HorizontalResolution;
            g_boot_info.fb.height   = mi->VerticalResolution;
            g_boot_info.fb.bpp      = 32;
            g_boot_info.fb.pitch    = mi->PixelsPerScanLine * 4;
            g_boot_info.fb.phys_addr = (UINT64)gop->Mode->FrameBufferBase;

            /* Определяем порядок каналов */
            if (mi->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
                g_boot_info.fb.red_mask   = 0x000000FF;
                g_boot_info.fb.green_mask = 0x0000FF00;
                g_boot_info.fb.blue_mask  = 0x00FF0000;
            } else if (mi->PixelFormat == PixelBitMask) {
                g_boot_info.fb.red_mask   = mi->PixelInformation.RedMask;
                g_boot_info.fb.green_mask = mi->PixelInformation.GreenMask;
                g_boot_info.fb.blue_mask  = mi->PixelInformation.BlueMask;
            } else {
                /* PixelBlueGreenRedReserved8BitPerColor (самый частый) и default */
                g_boot_info.fb.red_mask   = 0x00FF0000;
                g_boot_info.fb.green_mask = 0x0000FF00;
                g_boot_info.fb.blue_mask  = 0x000000FF;
            }

            DBG_VAL("BL", "fb.width",    g_boot_info.fb.width);
            DBG_VAL("BL", "fb.height",   g_boot_info.fb.height);
            DBG_VAL("BL", "fb.phys",     g_boot_info.fb.phys_addr);
            DBG_VAL("BL", "fb.pitch",    g_boot_info.fb.pitch);
            DBG_VAL("BL", "fb.pixel_fmt",(UINT64)mi->PixelFormat);
            bl_log(ConOut, "[OK] GOP framebuffer\r\n");
        } else {
            DBG_MSG("BL", "GOP not found — VGA fallback");
            bl_log(ConOut, "[WARN] GOP not found, VGA fallback\r\n");
        }
    }

    /* ==========================================================
       GetMemoryMap + ExitBootServices: MapKey устаревает после 
       ЛЮБОГО вызова Boot Service. Между ними не должно быть 
       НИЧЕГО, даже отладочного вывода.
       ========================================================== */
    DBG_MSG("BL", "09 GetMemoryMap + ExitBootServices...");
    MemoryMap = NULL;
    MemoryMapSize = 0;
    
    for (int attempt = 0; attempt < 32; attempt++) {
        if (MemoryMap) {
            BS->FreePool(MemoryMap);
            MemoryMap = NULL;
        }

        MemoryMapSize = 0;
        status = BS->GetMemoryMap(&MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
        if (status != EFI_BUFFER_TOO_SMALL && status != EFI_SUCCESS) {
            dbg_puts("[BL] GetMemoryMap size failed: ");
            dbg_hex64(status);
            dbg_puts("\n");
            for(;;) __asm__ volatile("hlt");
        }

        MemoryMapSize += 4 * DescriptorSize + PAGE_SIZE;
        status = BS->AllocatePool(EfiLoaderData, MemoryMapSize, (void **)&MemoryMap);
        if (status != EFI_SUCCESS) {
            dbg_puts("[BL] Alloc memmap pool failed: ");
            dbg_hex64(status);
            dbg_puts("\n");
            for(;;) __asm__ volatile("hlt");
        }

        UINTN new_size = MemoryMapSize;
        status = BS->GetMemoryMap(&new_size, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
        if (status == EFI_BUFFER_TOO_SMALL) {
            MemoryMapSize = new_size + DescriptorSize;
            continue;
        }
        if (status != EFI_SUCCESS) {
            dbg_puts("[BL] GetMemoryMap fill failed: "); // ИСПРАВЛЕНО: bl_ fail -> dbg_puts для безопасности
            dbg_hex64(status);
            dbg_puts("\n");
            for(;;) __asm__ volatile("hlt");
        }
        MemoryMapSize = new_size;

        // !!! СТРОГО НИКАКИХ ВЫЗОВОВ ЗДЕСЬ !!!
        // Ни bl_log, ни dbg_puts, ни Allocate, ни Free
        
        status = BS->ExitBootServices(ImageHandle, MapKey);
        if (status == EFI_SUCCESS) {
            break;
        }

        if (status != EFI_INVALID_PARAMETER) {
            dbg_puts("[BL] ExitBootServices fatal: ");
            dbg_hex64(status);
            dbg_puts("\n");
            for(;;) __asm__ volatile("hlt");
        }
    }

    if (status != EFI_SUCCESS) {
        DBG_PANIC("BL", "ExitBootServices failed after 32 attempts");
    }

    DBG_MSG("BL", "10 ExitBootServices OK");

    __asm__ volatile ("cli");
    g_boot_info.magic = BOOT_INFO_MAGIC;
    g_boot_info.mem_map = MemoryMap;
    g_boot_info.mem_map_size = MemoryMapSize;
    g_boot_info.desc_size = DescriptorSize;
    g_boot_info.desc_version = DescriptorVersion;
    g_boot_info.kernel_entry = elf->e_entry;
    g_boot_info.pml4_phys = pml4_phys;
    g_boot_info.gdt_ptr = NULL;
    g_boot_info.idt_ptr = NULL;
    g_boot_info.rsdp_phys = rsdp_phys;
    /* g_boot_info.fb уже заполнена выше (до ExitBootServices) */

    UINT64 rsp = (UINT64)(UINTN)(bl_exit_stack + sizeof(bl_exit_stack));
    jump_to_kernel_virt(pml4_phys, elf->e_entry, &g_boot_info, rsp); // ИСПРАВЛЕНО: убран пробел

    DBG_PANIC("BL", "jump returned");
    return EFI_SUCCESS;
}