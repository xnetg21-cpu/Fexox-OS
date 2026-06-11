/*
 * AHCI.c — AHCI/SATA драйвер для FEXOS
 *
 * Поддерживает:
 *   - AHCI 1.3 (PCI class 0x01, subclass 0x06, prog-if 0x01)
 *   - До 32 портов на контроллер, до AHCI_MAX_CONTROLLERS контроллеров
 *   - ATA устройства (HDD/SSD): LBA28 и LBA48
 *   - ATAPI определяется и пропускается
 *   - DMA через PRDT в Command Table (H2D FIS)
 *   - Регистрация через blkdev_register → "sda","sdb",…
 *
 * Архитектура памяти на порт (всё физически < 4 ГБ):
 *
 *   [Command List]  = 32 × 32 байт  = 1024 байт
 *   [FIS буфер]     = 256 байт
 *   [Command Table] = 128 байт header + 8 байт × PRDT_ENTRIES
 *
 *   Всё это кладётся в один DMA-буфер на порт (одна страница 4096 байт).
 *
 * Схема одного запроса (слот 0):
 *   1. Заполнить H2D Register FIS в Command Table
 *   2. Заполнить PRDT entry (физ. адрес буфера, размер)
 *   3. Заполнить Command List entry (slot 0): указать на Command Table,
 *      флаги write/read, число PRDT entries
 *   4. port->CI |= (1 << 0)  — выдать команду
 *   5. Polling: ждать пока port->CI & 1 == 0
 *   6. Проверить port->IS на ошибки
 */

#include "VFS.h"
#include "debug_out.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Утилиты
 * ========================================================================= */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

typedef uint64_t phys_addr_t;
extern phys_addr_t pmm_alloc_frames_above(uint64_t count, uint64_t align,
                                           phys_addr_t min_phys);
extern void        pmm_free_frames(phys_addr_t pa, uint64_t count);

#ifndef DIRECT_MAP_OFFSET
#define DIRECT_MAP_OFFSET 0xFFFF880000000000ULL
#endif
#define DMA_MIN_PHYS 0x100000ULL

static inline void *dma_alloc(uint64_t size) {
    uint64_t pages = (size + 4095) / 4096;
    phys_addr_t pa = pmm_alloc_frames_above(pages, 1, DMA_MIN_PHYS);
    if (pa == (phys_addr_t)-1) return NULL;
    void *va = (void *)(uintptr_t)(pa + DIRECT_MAP_OFFSET);
    uint8_t *p = (uint8_t *)va;
    for (uint64_t i = 0; i < pages * 4096; i++) p[i] = 0;
    return va;
}
static inline void dma_free(void *va, uint64_t size) {
    if (!va) return;
    phys_addr_t pa = (phys_addr_t)(uintptr_t)va - DIRECT_MAP_OFFSET;
    pmm_free_frames(pa, (size + 4095) / 4096);
}
static inline uint64_t virt_to_phys(const void *v) {
    uint64_t a = (uint64_t)(uintptr_t)v;
    if (a >= DIRECT_MAP_OFFSET)     return a - DIRECT_MAP_OFFSET;
    if (a >= 0xFFFFFFFF80000000ULL) return a - 0xFFFFFFFF80000000ULL + 0x200000ULL;
    return a;
}
static inline void *kzalloc(uint64_t size) {
    void *p = kmalloc(size);
    if (p) { uint8_t *b = (uint8_t *)p; for (uint64_t i = 0; i < size; i++) b[i] = 0; }
    return p;
}
static inline void kmem_cpy(void *d, const void *s, uint64_t n) {
    uint8_t *dd=(uint8_t*)d; const uint8_t *ss=(const uint8_t*)s;
    while(n--) *dd++=*ss++;
}
static inline int kstr_cmp(const char *a, const char *b) {
    while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b;
}
static inline void kstr_cpy(char *d, const char *s, int max) {
    int i=0; while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]=0;
}

/* IO port (для PCI config space) */
static inline uint32_t pci_read32(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t reg){
    uint32_t a=0x80000000U|((uint32_t)bus<<16)|((uint32_t)dev<<11)|((uint32_t)fn<<8)|(reg&0xFC);
    __asm__ volatile("outl %0,%1"::"a"(a),"Nd"((uint16_t)0xCF8));
    uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"((uint16_t)0xCFC)); return v;
}
static inline void pci_write32(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t reg,uint32_t v){
    uint32_t a=0x80000000U|((uint32_t)bus<<16)|((uint32_t)dev<<11)|((uint32_t)fn<<8)|(reg&0xFC);
    __asm__ volatile("outl %0,%1"::"a"(a),"Nd"((uint16_t)0xCF8));
    __asm__ volatile("outl %0,%1"::"a"(v),"Nd"((uint16_t)0xCFC));
}

/* MMIO helpers — всегда volatile, с барьером */
static inline uint32_t mmio_r32(uint64_t addr) {
    __asm__ volatile("mfence":::"memory");
    return *(volatile uint32_t *)(uintptr_t)(addr + DIRECT_MAP_OFFSET);
}
static inline void mmio_w32(uint64_t addr, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(addr + DIRECT_MAP_OFFSET) = v;
    __asm__ volatile("mfence":::"memory");
}

/* =========================================================================
 * AHCI регистры — HBA Memory (смещения от ABAR)
 * ========================================================================= */

/* Generic Host Control */
#define AHCI_CAP        0x000  /* Host Capabilities */
#define AHCI_GHC        0x004  /* Global Host Control */
#define AHCI_IS         0x008  /* Interrupt Status */
#define AHCI_PI         0x00C  /* Ports Implemented */
#define AHCI_VS         0x010  /* Version */
#define AHCI_CAP2       0x024  /* Capabilities Extended */

/* GHC bits */
#define GHC_AE          (1u << 31)  /* AHCI Enable */
#define GHC_MRSM        (1u << 2)   /* MSI Revert to Single Message */
#define GHC_IE          (1u << 1)   /* Interrupt Enable */
#define GHC_HR          (1u << 0)   /* HBA Reset */

/* CAP bits */
#define CAP_S64A        (1u << 31)  /* 64-bit addressing */
#define CAP_NCQ         (1u << 30)  /* Native Command Queuing */
#define CAP_NCS_SHIFT   8           /* Number of Command Slots - 1 */
#define CAP_NCS_MASK    0x1F
#define CAP_NP_MASK     0x1F        /* Number of Ports - 1 */

/* Port регистры — смещение от ABAR + 0x100 + port*0x80 */
#define PORT_BASE(p)    (0x100u + (uint32_t)(p) * 0x80u)

#define POFF_CLB        0x00  /* Command List Base (32-bit) */
#define POFF_CLBU       0x04  /* Command List Base Upper */
#define POFF_FB         0x08  /* FIS Base (32-bit) */
#define POFF_FBU        0x0C  /* FIS Base Upper */
#define POFF_IS         0x10  /* Interrupt Status */
#define POFF_IE         0x14  /* Interrupt Enable */
#define POFF_CMD        0x18  /* Command and Status */
#define POFF_TFD        0x20  /* Task File Data */
#define POFF_SIG        0x24  /* Signature */
#define POFF_SSTS       0x28  /* SATA Status */
#define POFF_SCTL       0x2C  /* SATA Control */
#define POFF_SERR       0x30  /* SATA Error */
#define POFF_SACT       0x34  /* SATA Active (NCQ) */
#define POFF_CI         0x38  /* Command Issue */

/* PORT CMD bits */
#define PCMD_ST         (1u <<  0)  /* Start DMA engine */
#define PCMD_SUD        (1u <<  1)  /* Spin-Up Device */
#define PCMD_POD        (1u <<  2)  /* Power On Device */
#define PCMD_FRE        (1u <<  4)  /* FIS Receive Enable */
#define PCMD_FR         (1u << 14)  /* FIS Receive Running */
#define PCMD_CR         (1u << 15)  /* Command List Running */
#define PCMD_ATAPI      (1u << 24)  /* Device is ATAPI */
#define PCMD_ICC_ACTIVE (1u << 28)  /* Interface Communication Control: Active */

/* PORT IS / IE bits */
#define PORT_IS_TFES    (1u << 30)  /* Task File Error Status */
#define PORT_IS_HBFS    (1u << 29)  /* Host Bus Fatal Error */
#define PORT_IS_HBDS    (1u << 28)  /* Host Bus Data Error */
#define PORT_IS_IFS     (1u << 27)  /* Interface Fatal Error */
#define PORT_IS_DHRS    (1u <<  0)  /* Device to Host Register FIS */
#define PORT_IS_PSS     (1u <<  1)  /* PIO Setup FIS */
#define PORT_IS_DSS     (1u <<  2)  /* DMA Setup FIS */
#define PORT_IS_FATAL   (PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_HBDS | PORT_IS_IFS)

/* TFD bits */
#define TFD_STS_BSY     0x80
#define TFD_STS_DRQ     0x08
#define TFD_STS_ERR     0x01

/* SSTS — ссылочное состояние устройства */
#define SSTS_DET_MASK   0x0F
#define SSTS_DET_PRESENT 0x03  /* устройство присутствует и связь установлена */
#define SSTS_IPM_MASK   0xF00
#define SSTS_IPM_ACTIVE 0x100

/* Сигнатуры устройств */
#define SIG_ATA         0x00000101  /* ATA */
#define SIG_ATAPI       0xEB140101  /* ATAPI */
#define SIG_SEMB        0xC33C0101  /* Enclosure Management Bridge */
#define SIG_PM          0x96690101  /* Port Multiplier */

/* =========================================================================
 * ATA команды и FIS типы
 * ========================================================================= */
#define FIS_TYPE_REG_H2D  0x27  /* Host to Device Register FIS */
#define FIS_TYPE_REG_D2H  0x34  /* Device to Host Register FIS */
#define FIS_TYPE_DMA_ACT  0x39
#define FIS_TYPE_DMA_SETUP 0x41
#define FIS_TYPE_DATA     0x46
#define FIS_TYPE_BIST     0x58
#define FIS_TYPE_PIO_SETUP 0x5F
#define FIS_TYPE_DEV_BITS 0xA1

#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_READ_DMA       0xC8
#define ATA_CMD_WRITE_DMA      0xCA
#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA
#define ATA_CMD_CACHE_FLUSH     0xE7

/* =========================================================================
 * Структуры DMA-буферов (должны быть в физической памяти < 4 ГБ)
 * ========================================================================= */

/*
 * H2D Register FIS — команда от хоста к устройству
 * Spec: SATA 3.2, §10.3.1
 */
typedef struct __attribute__((packed)) {
    uint8_t  fis_type;    /* FIS_TYPE_REG_H2D = 0x27 */
    uint8_t  pmport_c;   /* bits[3:0]=pm_port, bit[7]=C (1=command, 0=control) */
    uint8_t  command;    /* ATA команда */
    uint8_t  featurel;   /* Features (low) */

    uint8_t  lba0;       /* LBA bits 7:0 */
    uint8_t  lba1;       /* LBA bits 15:8 */
    uint8_t  lba2;       /* LBA bits 23:16 */
    uint8_t  device;     /* Device register (bit6=LBA mode) */

    uint8_t  lba3;       /* LBA bits 31:24 */
    uint8_t  lba4;       /* LBA bits 39:32 */
    uint8_t  lba5;       /* LBA bits 47:40 */
    uint8_t  featureh;   /* Features (high) */

    uint8_t  countl;     /* Sector count (low) */
    uint8_t  counth;     /* Sector count (high) */
    uint8_t  icc;        /* Isochronous Command Completion */
    uint8_t  control;    /* Device Control */

    uint8_t  rsv[4];
} fis_reg_h2d_t;

/*
 * PRDT Entry — Physical Region Descriptor Table entry
 * Один entry = один физически непрерывный регион памяти.
 */
typedef struct __attribute__((packed)) {
    uint32_t dba;        /* Data Base Address (32-bit) */
    uint32_t dbau;       /* Data Base Address Upper (для 64-bit) */
    uint32_t rsv;
    uint32_t dbc_i;      /* bits[21:0]=byte count-1, bit[31]=interrupt on completion */
} ahci_prdt_entry_t;

#define AHCI_PRDT_ENTRIES 8   /* достаточно для 8×64K = 512K за запрос */

/*
 * Command Table — заголовок команды + PRDT
 * Минимальный размер: 128 байт header + 16 байт × PRDT_ENTRIES
 */
typedef struct __attribute__((packed)) {
    uint8_t          cfis[64];   /* Command FIS (H2D FIS здесь) */
    uint8_t          acmd[16];   /* ATAPI command (не используем) */
    uint8_t          rsv[48];
    ahci_prdt_entry_t prdt[AHCI_PRDT_ENTRIES];
} ahci_cmd_tbl_t;

/*
 * Command List Entry — один слот (32 байта)
 * Массив из 32 таких записей = Command List (1024 байта).
 */
typedef struct __attribute__((packed)) {
    uint16_t flags;      /* bits[4:0]=FIS length/4, bit[6]=write, bit[10]=prefetch */
    uint16_t prdtl;      /* PRDT length (число entries) */
    uint32_t prdbc;      /* PRDT Byte Count (заполняется HBA) */
    uint32_t ctba;       /* Command Table Base Address (32-bit) */
    uint32_t ctbau;      /* Command Table Base Upper */
    uint32_t rsv[4];
} ahci_cmd_hdr_t;

/* Флаги cmd_hdr.flags */
#define CMDHDR_W    (1 << 6)   /* Write (host→device) */
#define CMDHDR_P    (1 << 7)   /* Prefetch PRDT */
#define CMDHDR_CLR  (1 << 10)  /* Clear BSY on R_OK */

/*
 * Весь DMA-буфер порта (влезает в 4096 байт):
 *   offset 0x000: Command List (32 × 32 = 1024 байт)
 *   offset 0x400: FIS Receive Buffer (256 байт)
 *   offset 0x500: Command Table слот 0 (128 + 8×16 = 256 байт)
 *   итого: 0x600 = 1536 байт < 4096 ✓
 */
#define PORT_BUF_CLB_OFF   0x000
#define PORT_BUF_FB_OFF    0x400
#define PORT_BUF_CTBL_OFF  0x500

/* =========================================================================
 * Структуры драйвера
 * ========================================================================= */
#define AHCI_MAX_CONTROLLERS 4
#define AHCI_MAX_PORTS       32
#define AHCI_MAX_DRIVES      (AHCI_MAX_CONTROLLERS * AHCI_MAX_PORTS)

typedef struct {
    uint64_t abar;        /* физический адрес ABAR (BAR5 HBA) */
    uint32_t ports_impl;  /* PI регистр — битовая маска портов */
    uint32_t num_slots;   /* число command slots (из CAP) */
    bool     s64a;        /* поддержка 64-bit адресации */
} ahci_hba_t;

typedef struct {
    ahci_hba_t  *hba;
    uint32_t     port_num;

    /* DMA-буфер порта */
    uint8_t     *buf;         /* виртуальный адрес */
    uint64_t     buf_phys;    /* физический */

    /* Указатели внутри buf */
    ahci_cmd_hdr_t *clb;      /* Command List Base */
    uint8_t        *fb;       /* FIS Buffer */
    ahci_cmd_tbl_t *ctbl;     /* Command Table (слот 0) */

    uint64_t     sectors;
    bool         lba48;
    char         model[41];
    blkdev_t     blkdev;
} ahci_port_t;

static ahci_hba_t  g_hbas[AHCI_MAX_CONTROLLERS];
static ahci_port_t g_ports[AHCI_MAX_DRIVES];
static int         g_hba_count  = 0;
static int         g_port_count = 0;

/* =========================================================================
 * Чтение/запись регистров порта
 * ========================================================================= */
static inline uint32_t port_r(ahci_hba_t *hba, uint32_t port, uint32_t off) {
    return mmio_r32(hba->abar + PORT_BASE(port) + off);
}
static inline void port_w(ahci_hba_t *hba, uint32_t port, uint32_t off, uint32_t v) {
    mmio_w32(hba->abar + PORT_BASE(port) + off, v);
}
static inline uint32_t hba_r(ahci_hba_t *hba, uint32_t off) {
    return mmio_r32(hba->abar + off);
}
static inline void hba_w(ahci_hba_t *hba, uint32_t off, uint32_t v) {
    mmio_w32(hba->abar + off, v);
}

/* =========================================================================
 * Остановка / запуск порта
 * ========================================================================= */

/* Останавливаем DMA engine и FIS receive */
static bool ahci_port_stop(ahci_hba_t *hba, uint32_t p) {
    uint32_t cmd = port_r(hba, p, POFF_CMD);

    /* Сначала ST=0 */
    cmd &= ~PCMD_ST;
    port_w(hba, p, POFF_CMD, cmd);

    /* Ждём CR=0 (Command List Running) */
    for (int i = 0; i < 500000; i++) {
        if (!(port_r(hba, p, POFF_CMD) & PCMD_CR)) break;
        __asm__ volatile("pause":::"memory");
    }

    /* FRE=0 */
    cmd = port_r(hba, p, POFF_CMD);
    cmd &= ~PCMD_FRE;
    port_w(hba, p, POFF_CMD, cmd);

    /* Ждём FR=0 (FIS Receive Running) */
    for (int i = 0; i < 500000; i++) {
        if (!(port_r(hba, p, POFF_CMD) & PCMD_FR)) return true;
        __asm__ volatile("pause":::"memory");
    }
    DBG_VAL("AHCI", "port_stop: FR still set for port", p);
    return false;
}

/* Запускаем порт */
static void ahci_port_start(ahci_hba_t *hba, uint32_t p) {
    /* Ждём BSY и DRQ = 0 */
    for (int i = 0; i < 500000; i++) {
        uint32_t tfd = port_r(hba, p, POFF_TFD);
        if (!(tfd & (TFD_STS_BSY | TFD_STS_DRQ))) break;
        __asm__ volatile("pause":::"memory");
    }

    uint32_t cmd = port_r(hba, p, POFF_CMD);
    cmd |= PCMD_FRE;
    port_w(hba, p, POFF_CMD, cmd);

    cmd = port_r(hba, p, POFF_CMD);
    cmd |= PCMD_ST;
    port_w(hba, p, POFF_CMD, cmd);
}

/* =========================================================================
 * Инициализация памяти порта
 * ========================================================================= */
static bool ahci_port_init_mem(ahci_hba_t *hba, ahci_port_t *ap) {
    /* Выделяем одну страницу под все буферы порта */
    ap->buf = (uint8_t *)dma_alloc(4096);
    if (!ap->buf) { DBG_MSG("AHCI", "dma_alloc port buf failed"); return false; }

    ap->buf_phys = virt_to_phys(ap->buf);
    if (ap->buf_phys > 0xFFFFFFFFULL) {
        DBG_MSG("AHCI", "port buf phys > 4GB");
        dma_free(ap->buf, 4096);
        return false;
    }

    /* Раскладываем указатели */
    ap->clb  = (ahci_cmd_hdr_t *)(ap->buf + PORT_BUF_CLB_OFF);
    ap->fb   = ap->buf + PORT_BUF_FB_OFF;
    ap->ctbl = (ahci_cmd_tbl_t *)(ap->buf + PORT_BUF_CTBL_OFF);

    uint32_t p = ap->port_num;

    /* Сообщаем HBA адреса */
    port_w(hba, p, POFF_CLB,  (uint32_t)(ap->buf_phys + PORT_BUF_CLB_OFF));
    port_w(hba, p, POFF_CLBU, 0);
    port_w(hba, p, POFF_FB,   (uint32_t)(ap->buf_phys + PORT_BUF_FB_OFF));
    port_w(hba, p, POFF_FBU,  0);

    /* Command List slot 0 → Command Table */
    uint32_t ctbl_phys = (uint32_t)(ap->buf_phys + PORT_BUF_CTBL_OFF);
    ap->clb[0].ctba  = ctbl_phys;
    ap->clb[0].ctbau = 0;

    /* Очищаем IS */
    port_w(hba, p, POFF_IS, 0xFFFFFFFF);

    return true;
}

/* =========================================================================
 * IDENTIFY (ATA IDENTIFY через AHCI)
 * ========================================================================= */
static bool ahci_identify(ahci_port_t *ap, uint16_t *buf_256) {
    ahci_hba_t *hba = ap->hba;
    uint32_t    p   = ap->port_num;

    /* Выделяем 512-байтный буфер для ответа IDENTIFY */
    uint8_t *id_buf = (uint8_t *)dma_alloc(4096);
    if (!id_buf) return false;
    uint64_t id_phys = virt_to_phys(id_buf);
    if (id_phys > 0xFFFFFFFFULL) { dma_free(id_buf, 4096); return false; }

    /* Заполняем H2D FIS */
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)ap->ctbl->cfis;
    uint8_t *cfis = (uint8_t *)fis;
    for (int i = 0; i < 64; i++) cfis[i] = 0;

    fis->fis_type  = FIS_TYPE_REG_H2D;
    fis->pmport_c  = 0x80;              /* C=1: это команда */
    fis->command   = ATA_CMD_IDENTIFY;
    fis->device    = 0;
    fis->countl    = 1;

    /* PRDT: один entry → id_buf */
    ap->ctbl->prdt[0].dba   = (uint32_t)id_phys;
    ap->ctbl->prdt[0].dbau  = 0;
    ap->ctbl->prdt[0].rsv   = 0;
    ap->ctbl->prdt[0].dbc_i = 512 - 1;  /* byte count - 1 */

    /* Command Header (слот 0): FIS длина = 5 DW, PRDTL=1, W=0 (read) */
    ap->clb[0].flags  = (uint16_t)((sizeof(fis_reg_h2d_t) / 4) & 0x1F);
    ap->clb[0].prdtl  = 1;
    ap->clb[0].prdbc  = 0;
    /* ctba уже настроен в init_mem */

    __asm__ volatile("mfence":::"memory");

    /* Очищаем IS */
    port_w(hba, p, POFF_IS, 0xFFFFFFFF);
    port_w(hba, p, POFF_SERR, 0xFFFFFFFF);

    /* Выдаём команду: слот 0 */
    port_w(hba, p, POFF_CI, 1);

    /* Polling: ждём CI[0]=0 */
    bool ok = false;
    for (uint32_t spin = 0; spin < 10000000; spin++) {
        uint32_t is  = port_r(hba, p, POFF_IS);
        uint32_t ci  = port_r(hba, p, POFF_CI);
        if (is & PORT_IS_FATAL) {
            DBG_VAL("AHCI", "identify: fatal IS", is);
            break;
        }
        if (!(ci & 1)) { ok = true; break; }
        __asm__ volatile("pause":::"memory");
    }

    if (ok) {
        __asm__ volatile("mfence":::"memory");
        uint16_t *src = (uint16_t *)id_buf;
        for (int i = 0; i < 256; i++) buf_256[i] = src[i];
    }

    port_w(hba, p, POFF_IS, 0xFFFFFFFF);
    dma_free(id_buf, 4096);
    return ok;
}

/* =========================================================================
 * DMA Read / Write
 * ========================================================================= */
static int ahci_rw(ahci_port_t *ap, uint64_t lba,
                   uint32_t count, void *buf, bool write) {
    ahci_hba_t *hba = ap->hba;
    uint32_t    p   = ap->port_num;

    if (count == 0) return VFS_OK;

    /* Максимум за один запрос: AHCI_PRDT_ENTRIES × 64K / 512 */
    uint32_t max_sectors = (uint32_t)(AHCI_PRDT_ENTRIES * 65536) / 512;
    if (count > max_sectors) count = max_sectors;

    uint32_t byte_count = count * 512;

    /* DMA буфер (физически непрерывный, < 4 ГБ) */
    uint8_t *dma_buf = (uint8_t *)dma_alloc(byte_count + 4096);
    if (!dma_buf) { DBG_MSG("AHCI","rw: dma_alloc failed"); return VFS_ERR_NOMEM; }

    /* Выравниваем на 64K-границу */
    uint64_t raw_phys  = virt_to_phys(dma_buf);
    uint64_t aligned   = (raw_phys + 65535) & ~(uint64_t)65535;
    uint8_t *aligned_v = (uint8_t *)(uintptr_t)(aligned + DIRECT_MAP_OFFSET);

    if (aligned > 0xFFFFFFFFULL) {
        DBG_MSG("AHCI","rw: aligned buf > 4GB");
        dma_free(dma_buf, byte_count + 4096);
        return VFS_ERR_IO;
    }

    /* При записи — копируем данные в DMA-буфер */
    if (write) {
        uint8_t *src = (uint8_t *)buf;
        for (uint32_t i = 0; i < byte_count; i++) aligned_v[i] = src[i];
        __asm__ volatile("mfence":::"memory");
    }

    /* Строим PRDT: делим byte_count на чанки по 64K */
    uint32_t remain = byte_count;
    uint32_t offset = 0;
    uint32_t nprd   = 0;

    while (remain > 0 && nprd < AHCI_PRDT_ENTRIES) {
        uint32_t chunk = remain > 65536 ? 65536 : remain;
        ap->ctbl->prdt[nprd].dba   = (uint32_t)(aligned + offset);
        ap->ctbl->prdt[nprd].dbau  = 0;
        ap->ctbl->prdt[nprd].rsv   = 0;
        /* dbc = byte_count - 1; последний entry оставляем без interrupt */
        ap->ctbl->prdt[nprd].dbc_i = chunk - 1;
        remain -= chunk;
        offset += chunk;
        nprd++;
    }

    /* Заполняем H2D FIS */
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)ap->ctbl->cfis;
    uint8_t *cfis = (uint8_t *)fis;
    for (int i = 0; i < 64; i++) cfis[i] = 0;

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;  /* C=1 */

    bool lba48 = ap->lba48 && (lba >= 0x10000000ULL || count > 255);

    if (lba48) {
        fis->command  = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
        fis->device   = 0x40;  /* LBA mode */
        fis->lba0     = (uint8_t)(lba & 0xFF);
        fis->lba1     = (uint8_t)(lba >> 8);
        fis->lba2     = (uint8_t)(lba >> 16);
        fis->lba3     = (uint8_t)(lba >> 24);
        fis->lba4     = (uint8_t)(lba >> 32);
        fis->lba5     = (uint8_t)(lba >> 40);
        fis->countl   = (uint8_t)(count & 0xFF);
        fis->counth   = (uint8_t)(count >> 8);
    } else {
        fis->command  = write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA;
        fis->device   = (uint8_t)(0xE0 | ((lba >> 24) & 0x0F));
        fis->lba0     = (uint8_t)(lba & 0xFF);
        fis->lba1     = (uint8_t)(lba >> 8);
        fis->lba2     = (uint8_t)(lba >> 16);
        fis->lba3     = 0;
        fis->lba4     = 0;
        fis->lba5     = 0;
        fis->countl   = (uint8_t)(count & 0xFF);
        fis->counth   = 0;
    }

    /* Command Header (слот 0) */
    uint16_t flags = (uint16_t)((sizeof(fis_reg_h2d_t) / 4) & 0x1F);
    if (write) flags |= CMDHDR_W;
    ap->clb[0].flags  = flags;
    ap->clb[0].prdtl  = (uint16_t)nprd;
    ap->clb[0].prdbc  = 0;

    __asm__ volatile("mfence":::"memory");

    /* Очищаем IS и SERR */
    port_w(hba, p, POFF_IS,   0xFFFFFFFF);
    port_w(hba, p, POFF_SERR, 0xFFFFFFFF);

    /* Проверяем BSY/DRQ до выдачи команды */
    uint32_t tfd = port_r(hba, p, POFF_TFD);
    if (tfd & (TFD_STS_BSY | TFD_STS_DRQ)) {
        DBG_VAL("AHCI","rw: port busy before CI, TFD", tfd);
        dma_free(dma_buf, byte_count + 4096);
        return VFS_ERR_IO;
    }

    /* Выдаём команду */
    port_w(hba, p, POFF_CI, 1);

    /* Polling: ждём CI[0]=0 или ошибки */
    bool ok = false;
    for (uint32_t spin = 0; spin < 50000000; spin++) {
        uint32_t is = port_r(hba, p, POFF_IS);
        if (is & PORT_IS_FATAL) {
            DBG_VAL("AHCI","rw: fatal IS", is);
            DBG_VAL("AHCI","rw: TFD",  port_r(hba, p, POFF_TFD));
            DBG_VAL("AHCI","rw: SERR", port_r(hba, p, POFF_SERR));
            break;
        }
        if (!(port_r(hba, p, POFF_CI) & 1)) { ok = true; break; }
        __asm__ volatile("pause":::"memory");
    }

    port_w(hba, p, POFF_IS, 0xFFFFFFFF);

    if (!ok) {
        DBG_MSG("AHCI","rw: TIMEOUT or error");
        dma_free(dma_buf, byte_count + 4096);
        return VFS_ERR_IO;
    }

    /* При чтении — копируем из DMA-буфера */
    if (!write) {
        __asm__ volatile("mfence":::"memory");
        uint8_t *dst = (uint8_t *)buf;
        for (uint32_t i = 0; i < byte_count; i++) dst[i] = aligned_v[i];
    } else {
        /* Flush cache */
        fis_reg_h2d_t *fis2 = (fis_reg_h2d_t *)ap->ctbl->cfis;
        uint8_t *cfis2 = (uint8_t *)fis2;
        for (int i = 0; i < 64; i++) cfis2[i] = 0;
        fis2->fis_type = FIS_TYPE_REG_H2D;
        fis2->pmport_c = 0x80;
        fis2->command  = lba48 ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH;
        fis2->device   = 0;

        ap->clb[0].flags = (uint16_t)((sizeof(fis_reg_h2d_t) / 4) & 0x1F);
        ap->clb[0].prdtl = 0;
        ap->clb[0].prdbc = 0;
        __asm__ volatile("mfence":::"memory");

        port_w(hba, p, POFF_IS, 0xFFFFFFFF);
        port_w(hba, p, POFF_CI, 1);

        for (uint32_t spin = 0; spin < 10000000; spin++) {
            if (!(port_r(hba, p, POFF_CI) & 1)) break;
            __asm__ volatile("pause":::"memory");
        }
        port_w(hba, p, POFF_IS, 0xFFFFFFFF);
    }

    dma_free(dma_buf, byte_count + 4096);
    return VFS_OK;
}

/* =========================================================================
 * blkdev_ops
 * ========================================================================= */
static int ahci_blkdev_read(blkdev_t *dev, uint64_t lba,
                             uint32_t count, void *buf) {
    ahci_port_t *ap = (ahci_port_t *)dev->priv;
    if (lba + count > ap->sectors) return VFS_ERR_OVERFLOW;

    uint32_t max = (uint32_t)(AHCI_PRDT_ENTRIES * 65536) / 512;
    uint8_t *dst = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > max ? max : count;
        int r = ahci_rw(ap, lba, n, dst, false);
        if (r != VFS_OK) return r;
        lba   += n;
        dst   += (uint64_t)n * 512;
        count -= n;
    }
    return VFS_OK;
}

static int ahci_blkdev_write(blkdev_t *dev, uint64_t lba,
                              uint32_t count, const void *buf) {
    ahci_port_t *ap = (ahci_port_t *)dev->priv;
    if (dev->readonly)              return VFS_ERR_ROFS;
    if (lba + count > ap->sectors)  return VFS_ERR_OVERFLOW;

    uint32_t max = (uint32_t)(AHCI_PRDT_ENTRIES * 65536) / 512;
    const uint8_t *src = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > max ? max : count;
        int r = ahci_rw(ap, lba, n, (void *)src, true);
        if (r != VFS_OK) return r;
        lba   += n;
        src   += (uint64_t)n * 512;
        count -= n;
    }
    return VFS_OK;
}

static void ahci_blkdev_destroy(blkdev_t *dev) { (void)dev; }

static blkdev_ops_t g_ahci_ops = {
    .read_sectors  = ahci_blkdev_read,
    .write_sectors = ahci_blkdev_write,
    .destroy       = ahci_blkdev_destroy,
};

/* =========================================================================
 * Извлечение строки из IDENTIFY
 * ========================================================================= */
static void ahci_copy_string(char *dst, const uint16_t *words,
                               int word_start, int char_len) {
    int j = 0;
    for (int i = 0; i < char_len / 2; i++) {
        uint16_t w = words[word_start + i];
        dst[j++] = (char)(w >> 8);
        dst[j++] = (char)(w & 0xFF);
    }
    int last = char_len - 1;
    while (last >= 0 && dst[last] == ' ') last--;
    dst[last + 1] = '\0';
}

/* =========================================================================
 * Probe одного порта
 * ========================================================================= */
static const char *ahci_drive_name(int idx) {
    /* sda, sdb, ..., sdz, sdaa, ... */
    static char buf[8];
    if (idx < 26) {
        buf[0] = 's'; buf[1] = 'd'; buf[2] = (char)('a' + idx); buf[3] = 0;
    } else {
        int hi = idx / 26 - 1;
        int lo = idx % 26;
        buf[0]='s'; buf[1]='d';
        buf[2]=(char)('a'+hi); buf[3]=(char)('a'+lo); buf[4]=0;
    }
    return buf;
}

static void ahci_probe_port(ahci_hba_t *hba, uint32_t pnum) {
    /* Проверяем SSTS: устройство присутствует? */
    uint32_t ssts = port_r(hba, pnum, POFF_SSTS);
    uint8_t det = (uint8_t)(ssts & SSTS_DET_MASK);
    uint32_t ipm = ssts & SSTS_IPM_MASK;

    if (det != SSTS_DET_PRESENT || ipm != SSTS_IPM_ACTIVE) {
        return;  /* нет устройства на порту */
    }

    /* Проверяем сигнатуру */
    uint32_t sig = port_r(hba, pnum, POFF_SIG);
    if (sig == SIG_ATAPI || sig == SIG_SEMB || sig == SIG_PM) {
        DBG_VAL("AHCI", "port has non-ATA device, sig", sig);
        return;
    }

    if (g_port_count >= AHCI_MAX_DRIVES) {
        DBG_MSG("AHCI", "too many drives, skipping port");
        return;
    }

    ahci_port_t *ap = &g_ports[g_port_count];
    ap->hba      = hba;
    ap->port_num = pnum;

    DBG_VAL("AHCI", "ATA device on port", pnum);

    /* Останавливаем порт перед настройкой памяти */
    ahci_port_stop(hba, pnum);

    /* Инициализируем DMA-буферы */
    if (!ahci_port_init_mem(hba, ap)) {
        DBG_VAL("AHCI", "port_init_mem failed for port", pnum);
        return;
    }

    /* Запускаем порт */
    ahci_port_start(hba, pnum);

    /* Небольшая пауза — диск должен подготовиться */
    for (volatile uint32_t d = 0; d < 500000; d++) __asm__ volatile("pause");

    /* IDENTIFY */
    uint16_t id[256];
    if (!ahci_identify(ap, id)) {
        DBG_VAL("AHCI", "identify failed for port", pnum);
        ahci_port_stop(hba, pnum);
        dma_free(ap->buf, 4096);
        return;
    }

    /* LBA48 */
    ap->lba48 = !!(id[83] & (1 << 10));

    /* Количество секторов */
    if (ap->lba48) {
        ap->sectors  = (uint64_t)id[100];
        ap->sectors |= (uint64_t)id[101] << 16;
        ap->sectors |= (uint64_t)id[102] << 32;
        ap->sectors |= (uint64_t)id[103] << 48;
    } else {
        ap->sectors  = (uint32_t)id[60];
        ap->sectors |= (uint32_t)id[61] << 16;
    }

    /* Модель */
    ahci_copy_string(ap->model, id, 27, 40);

    /* Настраиваем blkdev */
    const char *name = ahci_drive_name(g_port_count);
    kstr_cpy(ap->blkdev.name, name, 32);
    ap->blkdev.sector_size  = 512;
    ap->blkdev.sector_count = ap->sectors;
    ap->blkdev.priv         = ap;
    ap->blkdev.ops          = &g_ahci_ops;
    ap->blkdev.readonly     = false;

    dbg_puts("[AHCI] port "); dbg_dec64(pnum);
    dbg_puts(" → "); dbg_puts(name);
    dbg_puts(" \""); dbg_puts(ap->model); dbg_puts("\"");
    DBG_VAL("AHCI", "  sectors", ap->sectors);
    DBG_VAL("AHCI", "  lba48",   ap->lba48);

    int r = blkdev_register(&ap->blkdev);
    if (r < 0) {
        DBG_VAL("AHCI", "blkdev_register failed", (uint64_t)(int64_t)r);
        return;
    }

    g_port_count++;
    DBG_MSG("AHCI", "  registered ok");
}

/* =========================================================================
 * Инициализация одного HBA
 * ========================================================================= */
static void ahci_init_hba(uint64_t abar_phys) {
    if (g_hba_count >= AHCI_MAX_CONTROLLERS) return;

    ahci_hba_t *hba = &g_hbas[g_hba_count++];
    hba->abar = abar_phys;

    /* Включаем AHCI режим */
    uint32_t ghc = hba_r(hba, AHCI_GHC);
    if (!(ghc & GHC_AE)) {
        hba_w(hba, AHCI_GHC, ghc | GHC_AE);
        ghc = hba_r(hba, AHCI_GHC);
    }

    /* Сброс HBA */
    hba_w(hba, AHCI_GHC, ghc | GHC_HR);
    for (int i = 0; i < 1000000; i++) {
        if (!(hba_r(hba, AHCI_GHC) & GHC_HR)) break;
        __asm__ volatile("pause":::"memory");
    }

    /* Снова включаем AHCI после сброса */
    hba_w(hba, AHCI_GHC, hba_r(hba, AHCI_GHC) | GHC_AE);

    /* Читаем CAP */
    uint32_t cap   = hba_r(hba, AHCI_CAP);
    hba->s64a      = !!(cap & CAP_S64A);
    hba->num_slots = ((cap >> CAP_NCS_SHIFT) & CAP_NCS_MASK) + 1;
    hba->ports_impl = hba_r(hba, AHCI_PI);

    uint32_t vs = hba_r(hba, AHCI_VS);
    DBG_VAL("AHCI", "HBA version",    vs);
    DBG_VAL("AHCI", "ports_impl",     hba->ports_impl);
    DBG_VAL("AHCI", "num_slots",      hba->num_slots);
    DBG_VAL("AHCI", "s64a",           hba->s64a);

    /* Сбрасываем глобальный IS */
    hba_w(hba, AHCI_IS, 0xFFFFFFFF);

    /* Опрашиваем реализованные порты */
    for (uint32_t i = 0; i < 32; i++) {
        if (hba->ports_impl & (1u << i))
            ahci_probe_port(hba, i);
    }
}

/* =========================================================================
 * Поиск AHCI контроллеров на PCI шине
 * ========================================================================= */
int ahci_init(void) {
    DBG_MSG("AHCI", "=== ahci_init ===");

    int found_hba = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, 0x00);
                if (id == 0xFFFFFFFF) { if (fn == 0) break; continue; }

                uint32_t cls = pci_read32((uint8_t)bus, dev, fn, 0x08);
                uint8_t class_code = (uint8_t)(cls >> 24);
                uint8_t subclass   = (uint8_t)(cls >> 16);
                uint8_t prog_if    = (uint8_t)(cls >> 8);

                /* AHCI: class=0x01, subclass=0x06, prog_if=0x01 */
                if (class_code != 0x01 || subclass != 0x06 || prog_if != 0x01)
                    continue;

                DBG_VAL("AHCI", "found AHCI HBA at bus", bus);
                DBG_VAL("AHCI", "  device", dev);
                DBG_VAL("AHCI", "  vendor:device", id);

                /* Включаем Bus Master + Memory Space */
                uint32_t pcmd = pci_read32((uint8_t)bus, dev, fn, 0x04);
                pci_write32((uint8_t)bus, dev, fn, 0x04, pcmd | 0x06);

                /* BAR5 = ABAR (AHCI Base Memory Register) */
                uint32_t bar5 = pci_read32((uint8_t)bus, dev, fn, 0x24);
                if (bar5 & 1) {
                    DBG_MSG("AHCI", "BAR5 is IO space, not AHCI MMIO — skip");
                    continue;
                }
                uint64_t abar = bar5 & ~0xFULL;

                /* 64-bit BAR? */
                if (((bar5 >> 1) & 3) == 2) {
                    uint32_t bar5h = pci_read32((uint8_t)bus, dev, fn, 0x28);
                    abar |= (uint64_t)bar5h << 32;
                }

                DBG_VAL("AHCI", "  ABAR phys", abar);
                ahci_init_hba(abar);
                found_hba++;

                /* Один function в слоте — break fn loop если нет multi-func */
                uint8_t hdr = (uint8_t)(pci_read32((uint8_t)bus, dev, fn, 0x0C) >> 16);
                if (fn == 0 && !(hdr & 0x80)) break;
            }
        }
    }

    if (found_hba == 0) {
        DBG_MSG("AHCI", "no AHCI controller found");
        return VFS_ERR_NXDEV;
    }

    DBG_VAL("AHCI", "total AHCI drives", g_port_count);

    if (g_port_count == 0) {
        DBG_MSG("AHCI", "no ATA drives on AHCI ports");
        return VFS_ERR_NXDEV;
    }

    DBG_MSG("AHCI", "=== ahci_init done ===");
    return VFS_OK;
}

/*
 * Найти AHCI blkdev по имени ("sda", "sdb", …)
 */
blkdev_t *ahci_find_drive(const char *name) {
    for (int i = 0; i < g_port_count; i++) {
        if (kstr_cmp(g_ports[i].blkdev.name, name) == 0)
            return &g_ports[i].blkdev;
    }
    return NULL;
}
