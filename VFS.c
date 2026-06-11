/*
 * VFS.c — Virtual File System core + Block Layer + Virtio-Blk driver
 * FEXOS — freestanding x86_64
 *
 * Содержит:
 *   1. Блочный слой: реестр устройств, вспомогательные функции
 *   2. Virtio-blk PCI драйвер (QEMU -device virtio-blk-pci)
 *   3. VFS ядро: реестр ФС, точки монтирования, таблица fd, path-resolver
 */

#include "VFS.h"
#include "debug_out.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Базовые утилиты (без libc)
 * ========================================================================= */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Прямое отображение физической памяти */
#ifndef DIRECT_MAP_OFFSET
#define DIRECT_MAP_OFFSET  0xFFFF880000000000ULL
#endif

/*
 * DMA-safe аллокатор: выделяет физически непрерывные страницы из PMM
 * (физический адрес гарантированно внутри реальной RAM QEMU),
 * возвращает виртуальный адрес через прямое отображение.
 *
 * Используется для virtio-буферов — kmalloc кладёт данные в heap,
 * который маппится на физические адреса > 4 GiB (вне RAM QEMU с -m 256M).
 */
typedef uint64_t phys_addr_t;
extern phys_addr_t pmm_alloc_frames(uint64_t count, uint64_t align);
extern void        pmm_free_frames(phys_addr_t pa, uint64_t count);
/*
 * pmm_alloc_frames_above — выделяет фреймы только начиная с min_phys.
 * Нужен для DMA: conventional memory (0–1MB) занята OVMF/QEMU-firmware.
 */
extern phys_addr_t pmm_alloc_frames_above(uint64_t count, uint64_t align,
                                           phys_addr_t min_phys);

/*
 * DMA_MIN_PHYS: нижняя граница для DMA-буферов.
 * Conventional memory (0x0–0xFFFFF) = 0–1MB: BIOS data area, OVMF structures,
 * ISA memory holes. QEMU не может надёжно делать DMA в эту зону при q35+OVMF.
 * Используем 1MB (0x100000) как безопасный старт.
 */
#define DMA_MIN_PHYS  0x100000ULL

static void *dma_alloc(uint64_t size) {
    uint64_t pages = (size + 4095) / 4096;
    phys_addr_t pa = pmm_alloc_frames_above(pages, 1, DMA_MIN_PHYS);
    if (pa == (phys_addr_t)-1) return NULL;
    /* Возвращаем виртуальный адрес через прямое отображение */
    void *va = (void *)(uintptr_t)(pa + DIRECT_MAP_OFFSET);
    /* Обнуляем */
    uint8_t *p = (uint8_t *)va;
    for (uint64_t i = 0; i < pages * 4096; i++) p[i] = 0;
    return va;
}

static void dma_free(void *va, uint64_t size) {
    if (!va) return;
    uint64_t pages = (size + 4095) / 4096;
    phys_addr_t pa = (phys_addr_t)(uintptr_t)va - DIRECT_MAP_OFFSET;
    pmm_free_frames(pa, pages);
}

static inline void *kzalloc(uint64_t size) {
    void *p = kmalloc(size);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        for (uint64_t i = 0; i < size; i++) b[i] = 0;
    }
    return p;
}

static inline void kmem_set(void *dst, uint8_t v, uint64_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = v;
}

static inline __attribute__((unused)) void kmem_cpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static inline int kstr_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int kstr_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int kstr_ncmp(const char *a, const char *b, int n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n < 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

static inline void kstr_cpy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Spinlock (UP: cli/sti) */
typedef volatile int spinlock_t;
static inline void spin_lock(spinlock_t *l)   { (void)l; __asm__ volatile("cli":::"memory"); }
static inline void spin_unlock(spinlock_t *l) { (void)l; __asm__ volatile("sti":::"memory"); }

/* =========================================================================
 * PCI утилиты (config space через IO ports 0xCF8/0xCFC)
 * ========================================================================= */
static inline uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t addr = 0x80000000U
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t val;
    __asm__ volatile ("inl %1, %0"  : "=a"(val) : "Nd"((uint16_t)0xCFC));
    return val;
}

static inline void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                                uint8_t reg, uint32_t val) {
    uint32_t addr = 0x80000000U
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ volatile ("outl %0, %1" :: "a"(val),  "Nd"((uint16_t)0xCFC));
}

static inline uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return (uint16_t)(pci_read32(bus, dev, fn, reg & ~2) >> ((reg & 2) * 8));
}

static inline __attribute__((unused)) uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return (uint8_t)(pci_read32(bus, dev, fn, reg & ~3) >> ((reg & 3) * 8));
}

/* =========================================================================
 * MMIO утилиты
 * ========================================================================= */
/* DIRECT_MAP_OFFSET определён выше */

static inline __attribute__((unused)) volatile uint32_t *mmio32(uint64_t phys) {
    return (volatile uint32_t *)(uintptr_t)(phys + DIRECT_MAP_OFFSET);
}

static inline __attribute__((unused)) void mmio_write32(uint64_t phys, uint32_t val) {
    volatile uint32_t *p = mmio32(phys);
    *p = val;
    __asm__ volatile ("mfence" ::: "memory");
}

static inline __attribute__((unused)) uint32_t mmio_read32(uint64_t phys) {
    __asm__ volatile ("mfence" ::: "memory");
    return *mmio32(phys);
}

/* IO port */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0,%1" :: "a"(val),"Nd"(port));
}
static inline __attribute__((unused)) uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0,%1" :: "a"(val),"Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v; __asm__ volatile ("inl %1,%0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0,%1" :: "a"(val),"Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v; __asm__ volatile ("inw %1,%0" : "=a"(v) : "Nd"(port)); return v;
}

/* Физический адрес виртуального указателя (прямое отображение) */
static inline uint64_t virt_to_phys(const void *v) {
    uint64_t a = (uint64_t)(uintptr_t)v;
    /* Если в зоне прямого отображения — вычитаем смещение */
    if (a >= DIRECT_MAP_OFFSET)
        return a - DIRECT_MAP_OFFSET;
    /* Если в higher-half ядра (0xFFFFFFFF80000000) */
    if (a >= 0xFFFFFFFF80000000ULL)
        return a - 0xFFFFFFFF80000000ULL + 0x200000ULL;
    return a;
}

/* =========================================================================
 * БЛОЧНЫЙ СЛОЙ
 * ========================================================================= */
static blkdev_t *g_blkdevs[VFS_MAX_BLKDEVS];
static int        g_blkdev_count = 0;
static spinlock_t g_blk_lock = 0;

int blkdev_register(blkdev_t *dev) {
    spin_lock(&g_blk_lock);
    if (g_blkdev_count >= VFS_MAX_BLKDEVS) {
        spin_unlock(&g_blk_lock);
        return VFS_ERR_BUSY;
    }
    int idx = g_blkdev_count++;
    g_blkdevs[idx] = dev;
    spin_unlock(&g_blk_lock);
    DBG_VAL("BLK", "registered device", (uint64_t)(uintptr_t)dev);
    dbg_puts("[BLK] device name: "); dbg_puts(dev->name); dbg_puts("\n");
    DBG_VAL("BLK", "sector_size", (uint64_t)dev->sector_size);
    DBG_VAL("BLK", "sector_count", dev->sector_count);
    return idx;
}

blkdev_t *blkdev_find(const char *name) {
    for (int i = 0; i < g_blkdev_count; i++) {
        if (g_blkdevs[i] && kstr_cmp(g_blkdevs[i]->name, name) == 0)
            return g_blkdevs[i];
    }
    return NULL;
}

int blkdev_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev || !dev->ops || !dev->ops->read_sectors)
        return VFS_ERR_INVAL;
    return dev->ops->read_sectors(dev, lba, count, buf);
}

int blkdev_write(blkdev_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!dev || !dev->ops || !dev->ops->write_sectors)
        return VFS_ERR_INVAL;
    if (dev->readonly)
        return VFS_ERR_ROFS;
    return dev->ops->write_sectors(dev, lba, count, buf);
}

/* =========================================================================
 * VIRTIO-BLK ДРАЙВЕР — полная переработка
 *
 * Транспорт: PCI legacy IO (BAR0), virtio 0.9/1.0-legacy.
 * QEMU: -device virtio-blk-pci,disable-modern=on,disable-legacy=off
 *
 * Подход: синхронный polling через clflush+lfence на used->idx.
 * Никаких прерываний, никакого CLI во время ожидания.
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Константы PCI / virtio
 * ------------------------------------------------------------------------- */
#define VIRTIO_VENDOR_ID        0x1AF4
#define VIRTIO_BLK_DEV_ID       0x1001   /* legacy */
#define VIRTIO_BLK_DEV_ID2      0x1042   /* modern transitional */

/* IO-регистры (BAR0, legacy) */
#define VREG_HOST_FEATURES      0x00
#define VREG_GUEST_FEATURES     0x04
#define VREG_QUEUE_PFN          0x08
#define VREG_QUEUE_SIZE         0x0C
#define VREG_QUEUE_SEL          0x0E
#define VREG_QUEUE_NOTIFY       0x10
#define VREG_STATUS             0x12
#define VREG_ISR                0x13
#define VREG_CONFIG             0x14     /* начало device-config */

/* Биты статуса */
#define VSTAT_ACK               0x01
#define VSTAT_DRIVER            0x02
#define VSTAT_DRIVER_OK         0x04
#define VSTAT_FEATURES_OK       0x08
#define VSTAT_FAILED            0x80

/* Фичи virtio-blk */
#define VBLK_F_RO               (1u << 5)
#define VBLK_F_BLK_SIZE         (1u << 6)

/* Типы запросов */
#define VBLK_T_IN               0
#define VBLK_T_OUT              1

/* Статус ответа от устройства */
#define VBLK_S_OK               0
#define VBLK_S_IOERR            1
#define VBLK_S_UNSUPP           2

/* Флаги дескриптора */
#define VDESC_F_NEXT            0x01
#define VDESC_F_WRITE           0x02    /* device-writable */

/* -------------------------------------------------------------------------
 * Структуры virtqueue
 * Virtio legacy PFN layout:
 *   [0     .. 2047] desc table   (128 * 16 bytes)
 *   [2048  .. 2309] avail ring   (4 + 128*2 + 2)
 *   [2310  .. 4095] padding
 *   [4096  .. 5129] used ring    (4 + 128*8 + 2)  <- page-aligned!
 * ------------------------------------------------------------------------- */
#define VQ_SIZE         256   /* must match device queue_size (QEMU default=256) */
#define SECTOR_SIZE     512

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
    uint16_t used_event;
} vq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t       flags;
    uint16_t       idx;
    vq_used_elem_t ring[VQ_SIZE];
    uint16_t       avail_event;
} vq_used_t;

/* Virtqueue memory layout for queue_size=256:
 *   desc table:  256 * 16 = 4096 bytes  (page 0, offset 0)
 *   avail ring:  4 + 256*2 + 2 = 518 bytes (page 1, offset 0)
 *   padding:     to fill page 1
 *   used ring:   4 + 256*8 + 2 = 2054 bytes (page 2, offset 0)
 *
 * virtio legacy PFN layout: device uses PFN to find desc at PFN*4096,
 * avail at PFN*4096 + 16*queue_size (= 4096 for qs=256),
 * used  at align_4096(avail_end) = PFN*4096 + 8192 = (PFN+2)*4096.
 *
 * So we need 3 contiguous pages: pg0=desc, pg1=avail, pg2=used.
 * We allocate them as one 12K block and split.
 */
typedef struct {
    vq_desc_t  desc[VQ_SIZE];   /* 4096 bytes for VQ_SIZE=256 */
} __attribute__((packed, aligned(4096))) vq_page0_t;

typedef struct {
    vq_avail_t avail;
    uint8_t    _pad[4096 - sizeof(vq_avail_t)];
} __attribute__((packed, aligned(4096))) vq_page1_t;

typedef struct {
    vq_used_t  used;
} __attribute__((packed, aligned(4096))) vq_page2_t;

/* Заголовок блочного запроса */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} vblk_req_hdr_t;

/* -------------------------------------------------------------------------
 * Приватные данные драйвера
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t     io_base;
    uint16_t     queue_size;

    /* Вся virtqueue память (один contiguous DMA-буфер) */
    uint8_t     *vq_base;   /* виртуальный адрес начала */
    uint32_t     vq_pages;  /* сколько страниц выделено */
    vq_page0_t  *pg0;       /* = vq_base (для virt_to_phys в таймауте) */

    /* convenience-указатели */
    vq_desc_t   *desc;
    vq_avail_t  *avail;
    vq_used_t   *used;

    uint16_t     last_used_idx;

    /* DMA-буфер для req_hdr + status (отдельная страница) */
    vblk_req_hdr_t *req_hdr;
    uint8_t        *req_status;
} vblk_priv_t;

/* -------------------------------------------------------------------------
 * Вспомогательные: IO с логом
 * ------------------------------------------------------------------------- */
static inline void vblk_status_write(uint16_t io_base, uint8_t val) {
    outb((uint16_t)(io_base + VREG_STATUS), val);
}

/* -------------------------------------------------------------------------
 * PCI: поиск virtio-blk
 * ------------------------------------------------------------------------- */
static bool vblk_pci_find(uint8_t *bus_out, uint8_t *dev_out, uint8_t *fn_out) {
    DBG_MSG("VBK", "pci_find: scanning bus 0..7");
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;   /* slot empty */
            uint16_t vid = (uint16_t)(id & 0xFFFF);
            uint16_t did = (uint16_t)(id >> 16);
            if (vid == VIRTIO_VENDOR_ID &&
                (did == VIRTIO_BLK_DEV_ID || did == VIRTIO_BLK_DEV_ID2)) {
                DBG_VAL("VBK", "pci_find: bus",   bus);
                DBG_VAL("VBK", "pci_find: dev",   dev);
                DBG_VAL("VBK", "pci_find: did",   did);
                *bus_out = (uint8_t)bus;
                *dev_out = dev;
                *fn_out  = 0;
                return true;
            }
        }
    }
    DBG_MSG("VBK", "pci_find: not found");
    return false;
}

/* -------------------------------------------------------------------------
 * Полный flush строк кеша для DMA-структур
 * ------------------------------------------------------------------------- */
static inline void vblk_flush_range(const void *start, uint32_t len) {
    const uint8_t *p = (const uint8_t *)start;
    const uint8_t *end = p + len;
    while (p < end) {
        __asm__ volatile ("clflush (%0)" :: "r"(p) : "memory");
        p += 64;
    }
    __asm__ volatile ("mfence" ::: "memory");
}

static inline uint16_t vblk_read_used_idx(vq_used_t *used) {
    /* used->idx — offset 2 в vq_used_t (после flags:u16) */
    volatile uint16_t *ptr = (volatile uint16_t *)((uint8_t *)used + 2);
    __asm__ volatile ("clflush (%0)" :: "r"(ptr) : "memory");
    __asm__ volatile ("lfence" ::: "memory");
    return *ptr;
}

/* -------------------------------------------------------------------------
 * Отправить один запрос и дождаться ответа (polling)
 * ------------------------------------------------------------------------- */
static int vblk_do_request(vblk_priv_t *p,
                            uint32_t type, uint64_t sector,
                            uint32_t count, void *data_buf) {
    uint32_t data_len = count * SECTOR_SIZE;

    /* per-request spam убран; раскомментируй для отладки: */
    /* DBG_VAL("VBK","sector",sector); DBG_VAL("VBK","count",count); */

    /* --- DMA bounce buffer ------------------------------------------------
     * data_buf может лежать в kernel heap (phys > 256 MB, вне RAM QEMU).
     * Аллоцируем временный буфер через PMM (<256 MB). */
    uint32_t dma_size = (data_len < 4096) ? 4096 : data_len;
    uint8_t *dma_buf  = (uint8_t *)dma_alloc(dma_size);
    if (!dma_buf) {
        DBG_MSG("VBK", "  ERR: dma_alloc failed");
        return VFS_ERR_NOMEM;
    }
    uint64_t dma_phys = virt_to_phys(dma_buf);

    /* Проверяем что буфер в нижних 4 ГБ (legacy DMA) */
    if (dma_phys > 0xFFFFFFFFULL) {
        DBG_VAL("VBK", "  ERR: dma_phys > 4GB!", dma_phys);
        dma_free(dma_buf, dma_size);
        return VFS_ERR_IO;
    }

    /* Для записи копируем и флашим */
    if (type == VBLK_T_OUT) {
        uint8_t *src = (uint8_t *)data_buf;
        for (uint32_t i = 0; i < data_len; i++) dma_buf[i] = src[i];
        vblk_flush_range(dma_buf, data_len);
        DBG_MSG("VBK", "  write: copied and flushed data to DMA buf");
    }

    /* --- req_hdr + status -------------------------------------------------
     * req_hdr и req_status уже в DMA-зоне (аллоцированы в init).
     * Сбрасываем статус в 0xFF — устройство запишет 0x00 при успехе. */
    p->req_hdr->type     = type;
    p->req_hdr->reserved = 0;
    p->req_hdr->sector   = sector;
    *p->req_status       = 0xFF;
    vblk_flush_range(p->req_hdr, sizeof(vblk_req_hdr_t) + 1);

    uint64_t hdr_phys    = virt_to_phys(p->req_hdr);
    uint64_t status_phys = virt_to_phys(p->req_status);

    if (hdr_phys > 0xFFFFFFFFULL || status_phys > 0xFFFFFFFFULL) {
        DBG_MSG("VBK", "  ERR: hdr/status phys > 4GB!");
        dma_free(dma_buf, dma_size);
        return VFS_ERR_IO;
    }

    /* --- Дескрипторы ------------------------------------------------------
     * Desc[0]: hdr (device-readable)   flags=NEXT
     * Desc[1]: data (bounce buf)        flags=WRITE|NEXT для IN, NEXT для OUT
     * Desc[2]: status (1 байт)          flags=WRITE
     */
    uint16_t di_hdr    = 0;
    uint16_t di_data   = 1;
    uint16_t di_status = 2;

    p->desc[di_hdr].addr  = hdr_phys;
    p->desc[di_hdr].len   = sizeof(vblk_req_hdr_t);
    p->desc[di_hdr].flags = VDESC_F_NEXT;
    p->desc[di_hdr].next  = di_data;

    p->desc[di_data].addr  = dma_phys;
    p->desc[di_data].len   = data_len;
    p->desc[di_data].flags = (type == VBLK_T_IN ? VDESC_F_WRITE : 0) | VDESC_F_NEXT;
    p->desc[di_data].next  = di_status;

    p->desc[di_status].addr  = status_phys;
    p->desc[di_status].len   = 1;
    p->desc[di_status].flags = VDESC_F_WRITE;
    p->desc[di_status].next  = 0;

    vblk_flush_range(&p->desc[di_hdr],    sizeof(vq_desc_t));
    vblk_flush_range(&p->desc[di_data],   sizeof(vq_desc_t));
    vblk_flush_range(&p->desc[di_status], sizeof(vq_desc_t));

    /* --- avail ring -------------------------------------------------------
     * avail->flags = 0: не подавляем уведомления от устройства.
     * Добавляем голову цепочки (di_hdr=0) в avail->ring[avail->idx % qs].
     * Затем атомарно инкрементируем avail->idx и флашим. */
    p->avail->flags = 0;

    uint16_t avail_slot = p->avail->idx % (uint16_t)p->queue_size;
    p->avail->ring[avail_slot] = di_hdr;

    __asm__ volatile ("mfence" ::: "memory");
    p->avail->idx = (uint16_t)(p->avail->idx + 1);

    /* Флашим весь avail ring */
    vblk_flush_range(p->avail, sizeof(vq_avail_t));

    /* used->idx до отправки */
    uint16_t used_before = vblk_read_used_idx(p->used);
    uint16_t cur_used;

    /* --- Queue notify ----------------------------------------------------- */
    outw((uint16_t)(p->io_base + VREG_QUEUE_NOTIFY), 0);

    /*
     * Читаем ISR ПОСЛЕ notify — сбрасываем latch прерывания у устройства.
     * Без этого при polling без IRQ-хендлера virtio держит линию активной,
     * и следующий запрос видит «старый» used->idx до завершения нового.
     */
    inb((uint16_t)(p->io_base + VREG_ISR));

    /*
     * Polling: outw notify обрабатывается QEMU синхронно в I/O thread,
     * used->idx обновляется за ~1-10 мкс (несколько тысяч pause).
     * HLT не используем — IRQ 4 от virtio не настроен в нашем IOAPIC.
     */
    uint32_t spin = 0;

    while (1) {
        cur_used = vblk_read_used_idx(p->used);
        if (cur_used != used_before) break;

        __asm__ volatile ("pause" ::: "memory");
        spin++;

        if (spin >= 50000000) {
            /* Таймаут — дамп состояния для диагностики */
            DBG_MSG("VBK", "TIMEOUT: virtio never responded — state dump:");
            DBG_VAL("VBK", "  io_base",       p->io_base);
            DBG_VAL("VBK", "  used->idx",      cur_used);
            DBG_VAL("VBK", "  used_before",    used_before);
            DBG_VAL("VBK", "  last_used_idx",  p->last_used_idx);
            DBG_VAL("VBK", "  avail->idx",     p->avail->idx);
            DBG_VAL("VBK", "  ISR",     inb((uint16_t)(p->io_base + VREG_ISR)));
            DBG_VAL("VBK", "  STATUS",  inb((uint16_t)(p->io_base + VREG_STATUS)));
            DBG_VAL("VBK", "  req_status byte", *p->req_status);
            outw((uint16_t)(p->io_base + VREG_QUEUE_SEL), 0);
            DBG_VAL("VBK", "  PFN readback",
                    inl((uint16_t)(p->io_base + VREG_QUEUE_PFN)));
            DBG_VAL("VBK", "  pg0 phys",  virt_to_phys(p->pg0));
            DBG_VAL("VBK", "  avail phys",virt_to_phys(p->avail));
            DBG_VAL("VBK", "  used phys", virt_to_phys(p->used));
            dma_free(dma_buf, dma_size);
            return VFS_ERR_IO;
        }
    }

request_done:
    p->last_used_idx = cur_used;

    /* Читаем used ring element */
    uint16_t used_elem_idx = (uint16_t)((cur_used - 1) % p->queue_size);
    vblk_flush_range(&p->used->ring[used_elem_idx], sizeof(vq_used_elem_t));

    /* Читаем статус */
    __asm__ volatile ("clflush (%0)" :: "r"(p->req_status) : "memory");
    __asm__ volatile ("lfence"       ::: "memory");
    uint8_t status_byte = *p->req_status;
    DBG_VAL("VBK", "  req_status byte", status_byte);

    if (status_byte != VBLK_S_OK) {
        DBG_VAL("VBK", "ERR: device status", status_byte);
        dma_free(dma_buf, dma_size);
        return VFS_ERR_IO;
    }

    /* Для чтения — копируем из DMA-буфера в data_buf */
    if (type == VBLK_T_IN) {
        vblk_flush_range(dma_buf, data_len);
        uint8_t *dst = (uint8_t *)data_buf;
        for (uint32_t i = 0; i < data_len; i++) dst[i] = dma_buf[i];
    }

    dma_free(dma_buf, dma_size);
    return VFS_OK;
}

/* -------------------------------------------------------------------------
 * blkdev_ops: read / write
 * ------------------------------------------------------------------------- */
static int vblk_read_sectors(blkdev_t *dev, uint64_t lba,
                              uint32_t count, void *buf) {
    vblk_priv_t *p = (vblk_priv_t *)dev->priv;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        int r = vblk_do_request(p, VBLK_T_IN, lba + i, 1,
                                 dst + (uint64_t)i * SECTOR_SIZE);
        if (r != VFS_OK) {
            DBG_VAL("VBK", "read_sectors FAILED lba", lba + i);
            return r;
        }
    }
    return VFS_OK;
}

static int vblk_write_sectors(blkdev_t *dev, uint64_t lba,
                               uint32_t count, const void *buf) {
    vblk_priv_t *p = (vblk_priv_t *)dev->priv;
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        int r = vblk_do_request(p, VBLK_T_OUT, lba + i, 1,
                                 (void *)(src + (uint64_t)i * SECTOR_SIZE));
        if (r != VFS_OK) {
            DBG_VAL("VBK", "write_sectors FAILED lba", lba + i);
            return r;
        }
    }
    return VFS_OK;
}

static blkdev_ops_t g_virtio_blk_ops = {
    .read_sectors  = vblk_read_sectors,
    .write_sectors = vblk_write_sectors,
    .destroy       = NULL,
};

/* -------------------------------------------------------------------------
 * virtio_blk_init — полная инициализация
 * ------------------------------------------------------------------------- */
int virtio_blk_init(void) {
    DBG_MSG("VBK", "=== virtio_blk_init START ===");

    /* 1. Найти устройство на PCI */
    uint8_t bus = 0, dev_n = 0, fn = 0;
    if (!vblk_pci_find(&bus, &dev_n, &fn)) {
        DBG_MSG("VBK", "ERR: device not found");
        return VFS_ERR_NXDEV;
    }

    uint16_t pci_cmd = pci_read16(bus, dev_n, fn, 0x04);
    uint16_t pci_did = pci_read16(bus, dev_n, fn, 0x02);
    uint8_t  pci_rev = (uint8_t)(pci_read32(bus, dev_n, fn, 0x08) & 0xFF);
    DBG_VAL("VBK", "PCI did", pci_did);
    DBG_VAL("VBK", "PCI rev", pci_rev);

    /* Включаем IO Space + Bus Master */
    pci_write32(bus, dev_n, fn, 0x04, (uint32_t)(pci_cmd | 0x05));

    /* 2. BAR0 — IO base */
    uint32_t bar0 = pci_read32(bus, dev_n, fn, 0x10);
    DBG_VAL("VBK", "BAR0 raw", bar0);
    if (!(bar0 & 1)) {
        DBG_MSG("VBK", "ERR: BAR0 is MMIO, expected IO");
        return VFS_ERR_NXDEV;
    }
    uint16_t io_base = (uint16_t)(bar0 & 0xFFFC);
    DBG_VAL("VBK", "io_base", io_base);

    /* 3. Сброс устройства */
    vblk_status_write(io_base, 0);
    for (volatile int d = 0; d < 100000; d++) __asm__ volatile ("pause");

    /* 4-5. ACK + DRIVER */
    vblk_status_write(io_base, VSTAT_ACK);
    vblk_status_write(io_base, VSTAT_ACK | VSTAT_DRIVER);

    /* 6. Feature negotiation */
    uint32_t host_feat = inl((uint16_t)(io_base + VREG_HOST_FEATURES));
    DBG_VAL("VBK", "host_features", host_feat);
    uint32_t guest_feat = host_feat & (VBLK_F_RO | VBLK_F_BLK_SIZE);
    outl((uint16_t)(io_base + VREG_GUEST_FEATURES), guest_feat);
    bool readonly = !!(host_feat & VBLK_F_RO);

    /* FEATURES_OK только для modern transitional */
    bool features_ok_set = false;
    if (pci_did == VIRTIO_BLK_DEV_ID2) {
        vblk_status_write(io_base, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK);
        uint8_t sc = inb((uint16_t)(io_base + VREG_STATUS));
        if (sc & VSTAT_FEATURES_OK) {
            features_ok_set = true;
        } else {
            DBG_MSG("VBK", "FEATURES_OK rejected");
            vblk_status_write(io_base, VSTAT_ACK | VSTAT_DRIVER);
        }
    }

    /* 7. Device config: capacity */
    uint64_t cap_lo   = inl((uint16_t)(io_base + VREG_CONFIG));
    uint64_t cap_hi   = inl((uint16_t)(io_base + VREG_CONFIG + 4));
    uint64_t capacity = cap_lo | (cap_hi << 32);
    DBG_VAL("VBK", "capacity_sectors", capacity);

    /* 8. Virtqueue setup */
    DBG_MSG("VBK", "step8: virtqueue");
    outw((uint16_t)(io_base + VREG_QUEUE_SEL), 0);
    uint16_t qs = inw((uint16_t)(io_base + VREG_QUEUE_SIZE));
    DBG_VAL("VBK", "  queue_size", qs);
    if (qs == 0) {
        DBG_MSG("VBK", "ERR: queue_size == 0");
        return VFS_ERR_IO;
    }
    if (qs > VQ_SIZE) {
        DBG_VAL("VBK", "ERR: queue_size > VQ_SIZE; increase VQ_SIZE to", qs);
        return VFS_ERR_IO;
    }

    /* Аллоцируем virtqueue.
     *
     * virtio legacy PFN layout (spec §2.4.2):
     *   desc  @ PFN*4096 + 0
     *   avail @ PFN*4096 + 16*qs
     *   used  @ PFN*4096 + align_4096(16*qs + 4 + 2*qs + 2)
     *
     * Вычисляем размер динамически по реальному qs, а не VQ_SIZE=256,
     * иначе QEMU и драйвер расходятся в адресе used ring.
     */
    uint32_t avail_off  = 16u * qs;
    uint32_t avail_size = 4u + 2u * qs + 2u;
    uint32_t used_off   = (avail_off + avail_size + 4095u) & ~4095u;
    uint32_t used_size  = 4u + 8u * qs + 2u;
    uint32_t vq_total   = used_off + used_size;
    uint32_t vq_pages   = (vq_total + 4095u) / 4096u;

    DBG_VAL("VBK", "  qs",        qs);
    DBG_VAL("VBK", "  avail_off", avail_off);
    DBG_VAL("VBK", "  used_off",  used_off);
    DBG_VAL("VBK", "  vq_pages",  vq_pages);

    uint8_t *contig = (uint8_t *)dma_alloc((uint64_t)vq_pages * 4096);
    if (!contig) { DBG_MSG("VBK", "ERR: dma_alloc vq"); return VFS_ERR_NOMEM; }
    uint64_t pg0_phys = virt_to_phys(contig);
    DBG_VAL("VBK", "  contig base phys", pg0_phys);
    if (pg0_phys & 0xFFF) {
        DBG_VAL("VBK", "ERR: contig not page-aligned!", pg0_phys);
        dma_free(contig, (uint64_t)vq_pages * 4096);
        return VFS_ERR_IO;
    }

    /* DMA буфер для req_hdr + status */
    uint8_t *req_buf = (uint8_t *)dma_alloc(4096);
    if (!req_buf) {
        DBG_MSG("VBK", "ERR: dma_alloc req_buf");
        dma_free(contig, (uint64_t)vq_pages * 4096);
        return VFS_ERR_NOMEM;
    }
    uint64_t req_buf_phys = virt_to_phys(req_buf);
    DBG_VAL("VBK", "  req_buf phys", req_buf_phys);

    /* Инициализируем priv */
    vblk_priv_t *priv = (vblk_priv_t *)kzalloc(sizeof(vblk_priv_t));
    if (!priv) {
        DBG_MSG("VBK","ERR: kzalloc priv");
        dma_free(req_buf, 4096);
        dma_free(contig, (uint64_t)vq_pages * 4096);
        return VFS_ERR_NOMEM;
    }

    priv->io_base       = io_base;
    priv->queue_size    = qs;
    priv->vq_pages      = vq_pages;
    priv->vq_base       = contig;
    /* Desc, avail, used — по вычисленным смещениям */
    priv->desc          = (vq_desc_t *)contig;
    priv->avail         = (vq_avail_t *)(contig + avail_off);
    priv->used          = (vq_used_t  *)(contig + used_off);
    /* pg0 указывает на начало (нужен только для virt_to_phys в таймауте) */
    priv->pg0           = (vq_page0_t *)contig;
    priv->last_used_idx = 0;
    priv->req_hdr       = (vblk_req_hdr_t *)req_buf;
    priv->req_status    = req_buf + sizeof(vblk_req_hdr_t);

    DBG_VAL("VBK", "  desc  phys", pg0_phys);
    DBG_VAL("VBK", "  avail phys", pg0_phys + avail_off);
    DBG_VAL("VBK", "  used  phys", pg0_phys + used_off);

    /* Инициализируем avail ring */
    priv->avail->flags = 0;
    priv->avail->idx   = 0;

    /* Флашим всю virtqueue память перед регистрацией */
    vblk_flush_range(contig, (uint64_t)vq_pages * 4096);

    /*
     * Сообщаем устройству PFN страницы 0 (начало desc).
     * Устройство само вычислит avail и used по той же формуле что мы выше.
     */
    uint32_t pfn = (uint32_t)(pg0_phys >> 12);
    DBG_VAL("VBK", "  writing PFN", pfn);
    __asm__ volatile ("mfence" ::: "memory");
    outl((uint16_t)(io_base + VREG_QUEUE_PFN), pfn);

    /* Readback PFN */
    outw((uint16_t)(io_base + VREG_QUEUE_SEL), 0);
    uint32_t pfn_rb = inl((uint16_t)(io_base + VREG_QUEUE_PFN));
    DBG_VAL("VBK", "  PFN readback", pfn_rb);
    if (pfn_rb != pfn) {
        DBG_VAL("VBK", "ERR: PFN readback mismatch! expected", pfn);
        dma_free(contig, (uint64_t)vq_pages * 4096);
        dma_free(req_buf, 4096);
        return VFS_ERR_IO;
    }

    /* 9. DRIVER_OK */
    uint8_t final_status = VSTAT_ACK | VSTAT_DRIVER | VSTAT_DRIVER_OK;
    if (features_ok_set) final_status |= VSTAT_FEATURES_OK;
    vblk_status_write(io_base, final_status);

    uint8_t s_final = inb((uint16_t)(io_base + VREG_STATUS));
    if (s_final & VSTAT_FAILED) {
        DBG_MSG("VBK", "ERR: device FAILED after DRIVER_OK");
        return VFS_ERR_IO;
    }
    DBG_MSG("VBK", "=== virtio-blk READY ===");

    /* 10. Регистрируем blkdev */
    blkdev_t *blkdev = (blkdev_t *)kzalloc(sizeof(blkdev_t));
    if (!blkdev) return VFS_ERR_NOMEM;

    kstr_cpy(blkdev->name, "vda", 32);
    blkdev->sector_size  = SECTOR_SIZE;
    blkdev->sector_count = capacity;
    blkdev->priv         = priv;
    blkdev->ops          = &g_virtio_blk_ops;
    blkdev->readonly     = readonly;

    int idx = blkdev_register(blkdev);
    if (idx < 0) {
        DBG_VAL("VBK", "ERR: blkdev_register failed", (uint64_t)(int64_t)idx);
        return idx;
    }
    DBG_MSG("VBK", "vda registered OK");
    return VFS_OK;
}

/* =========================================================================
 * VFS ЯДРО
 * ========================================================================= */

/* Реестр файловых систем */
static fs_ops_t  *g_fs_drivers[16];
static int        g_fs_count = 0;

/* Таблица точек монтирования */
static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static int         g_mount_count = 0;
static spinlock_t  g_mount_lock = 0;

/* Файловый дескриптор */
typedef struct {
    vfs_node_t *node;
    uint64_t    offset;
    int         flags;
    bool        used;
} vfs_fd_t;

static vfs_fd_t   g_fds[VFS_MAX_FD];
static spinlock_t g_fd_lock = 0;

int vfs_init(void) {
    kmem_set(g_fs_drivers, 0, sizeof(g_fs_drivers));
    kmem_set(g_mounts,     0, sizeof(g_mounts));
    kmem_set(g_fds,        0, sizeof(g_fds));
    g_fs_count    = 0;
    g_mount_count = 0;
    DBG_MSG("VFS", "vfs_init ok");
    return VFS_OK;
}

int vfs_register_fs(fs_ops_t *ops) {
    if (g_fs_count >= 16) return VFS_ERR_BUSY;
    g_fs_drivers[g_fs_count++] = ops;
    DBG_MSG("VFS", "fs registered");
    return VFS_OK;
}

/* Найти драйвер ФС по имени */
static fs_ops_t *vfs_find_fs(const char *name) {
    for (int i = 0; i < g_fs_count; i++)
        if (g_fs_drivers[i] && kstr_cmp(g_fs_drivers[i]->name, name) == 0)
            return g_fs_drivers[i];
    return NULL;
}

/* Найти точку монтирования для пути (самая длинная совпадающая) */
static vfs_mount_t *vfs_find_mount(const char *path) {
    vfs_mount_t *best = NULL;
    int best_len = -1;
    for (int i = 0; i < g_mount_count; i++) {
        if (!g_mounts[i].root) continue;
        int mlen = kstr_len(g_mounts[i].path);
        if (kstr_ncmp(path, g_mounts[i].path, mlen) == 0) {
            /* Граница: либо конец строки, либо '/' */
            char next = path[mlen];
            if (next == '\0' || next == '/' || g_mounts[i].path[mlen - 1] == '/') {
                if (mlen > best_len) { best_len = mlen; best = &g_mounts[i]; }
            }
        }
    }
    return best;
}

int vfs_mount(const char *dev_name, const char *fs_name,
              const char *path, uint32_t flags) {
    blkdev_t *dev = blkdev_find(dev_name);
    if (!dev) {
        DBG_MSG("VFS", "vfs_mount: device not found");
        return VFS_ERR_NXDEV;
    }
    fs_ops_t *fs = vfs_find_fs(fs_name);
    if (!fs) {
        DBG_MSG("VFS", "vfs_mount: fs not found");
        return VFS_ERR_UNSUP;
    }

    spin_lock(&g_mount_lock);
    if (g_mount_count >= VFS_MAX_MOUNTS) {
        spin_unlock(&g_mount_lock);
        return VFS_ERR_BUSY;
    }

    DBG_MSG("VFS", "vfs_mount: calling fs->mount...");
    dbg_puts("[VFS] fs name: "); dbg_puts(fs->name); dbg_puts("\n");
    dbg_puts("[VFS] dev name: "); dbg_puts(dev->name); dbg_puts("\n");
    DBG_VAL("VFS", "dev sector_size",  (uint64_t)dev->sector_size);
    DBG_VAL("VFS", "dev sector_count", dev->sector_count);
    vfs_node_t *root = fs->mount(dev, flags);
    if (!root) {
        spin_unlock(&g_mount_lock);
        DBG_MSG("VFS", "vfs_mount: fs->mount failed");
        return VFS_ERR_IO;
    }

    vfs_mount_t *mnt = &g_mounts[g_mount_count++];
    kstr_cpy(mnt->path, path, VFS_MAX_PATH);
    mnt->root  = root;
    mnt->dev   = dev;
    mnt->fs    = fs;
    mnt->flags = flags;
    root->mount = mnt;
    spin_unlock(&g_mount_lock);

    DBG_MSG("VFS", "mounted ok");
    return VFS_OK;
}

int vfs_umount(const char *path) {
    spin_lock(&g_mount_lock);
    for (int i = 0; i < g_mount_count; i++) {
        if (kstr_cmp(g_mounts[i].path, path) == 0) {
            if (g_mounts[i].fs && g_mounts[i].fs->umount)
                g_mounts[i].fs->umount(&g_mounts[i]);
            if (g_mounts[i].root && g_mounts[i].root->ops->release)
                g_mounts[i].root->ops->release(g_mounts[i].root);
            kmem_set(&g_mounts[i], 0, sizeof(vfs_mount_t));
            /* Сдвигаем таблицу */
            for (int j = i; j < g_mount_count - 1; j++)
                g_mounts[j] = g_mounts[j + 1];
            g_mount_count--;
            spin_unlock(&g_mount_lock);
            return VFS_OK;
        }
    }
    spin_unlock(&g_mount_lock);
    return VFS_ERR_NOENT;
}

/* -------------------------------------------------------------------------
 * Path resolver: разбивает путь на компоненты и обходит дерево
 * ------------------------------------------------------------------------- */
static vfs_node_t *vfs_resolve(const char *path, bool parent, char *last_component) {
    vfs_mount_t *mnt = vfs_find_mount(path);
    if (!mnt) return NULL;

    /* Путь относительно точки монтирования */
    int mlen = kstr_len(mnt->path);
    const char *rel = path + mlen;
    /* Пропускаем ведущий '/' */
    while (*rel == '/') rel++;

    vfs_node_t *cur = mnt->root;
    cur->refcount++;

    if (*rel == '\0') {
        /* Запрашивают саму точку монтирования */
        if (parent && last_component) {
            last_component[0] = '.';
            last_component[1] = '\0';
        }
        return cur;
    }

    /* Разбираем компоненты */
    char comp[VFS_MAX_NAME + 1];
    const char *p = rel;

    while (*p) {
        /* Берём следующий компонент */
        int ci = 0;
        while (*p && *p != '/') {
            if (ci < VFS_MAX_NAME) comp[ci++] = *p;
            p++;
        }
        comp[ci] = '\0';
        while (*p == '/') p++;

        /* Если parent=true и это последний компонент — возвращаем cur */
        if (parent && *p == '\0') {
            if (last_component) kstr_cpy(last_component, comp, VFS_MAX_NAME + 1);
            return cur;
        }

        /* Ищем в текущей директории */
        if (!cur->ops || !cur->ops->lookup) {
            if (cur->ops && cur->ops->release) cur->ops->release(cur);
            return NULL;
        }
        vfs_node_t *next = cur->ops->lookup(cur, comp);
        if (cur->ops->release) cur->ops->release(cur);
        if (!next) return NULL;
        cur = next;
    }

    return cur;
}

/* -------------------------------------------------------------------------
 * Файловые дескрипторы
 * ------------------------------------------------------------------------- */
static int fd_alloc(vfs_node_t *node, int flags) {
    spin_lock(&g_fd_lock);
    for (int i = 3; i < VFS_MAX_FD; i++) {  /* 0,1,2 зарезервированы */
        if (!g_fds[i].used) {
            g_fds[i].node   = node;
            g_fds[i].offset = 0;
            g_fds[i].flags  = flags;
            g_fds[i].used   = true;
            spin_unlock(&g_fd_lock);
            return i;
        }
    }
    spin_unlock(&g_fd_lock);
    return VFS_ERR_NFILE;
}

static vfs_fd_t *fd_get(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FD || !g_fds[fd].used)
        return NULL;
    return &g_fds[fd];
}

/* -------------------------------------------------------------------------
 * VFS API
 * ------------------------------------------------------------------------- */
int vfs_open(const char *path, int flags) {
    bool create  = !!(flags & O_CREAT);
    bool is_dir  = !!(flags & O_DIRECTORY);

    vfs_node_t *node;

    if (create) {
        char last[VFS_MAX_NAME + 1];
        vfs_node_t *parent = vfs_resolve(path, true, last);
        if (!parent) return VFS_ERR_NOENT;

        /* Проверяем существование */
        node = parent->ops->lookup(parent, last);
        if (!node) {
            /* Создаём */
            if (!parent->ops->create) {
                if (parent->ops->release) parent->ops->release(parent);
                return VFS_ERR_UNSUP;
            }
            node = parent->ops->create(parent, last,
                                        is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE);
            if (!node) {
                if (parent->ops->release) parent->ops->release(parent);
                return VFS_ERR_IO;
            }
        }
        if (parent->ops->release) parent->ops->release(parent);
    } else {
        node = vfs_resolve(path, false, NULL);
        if (!node) return VFS_ERR_NOENT;
    }

    /* Проверка тип/режим */
    if (is_dir && node->stat.type != VFS_TYPE_DIR) {
        if (node->ops->release) node->ops->release(node);
        return VFS_ERR_NOTDIR;
    }
    if (!is_dir && node->stat.type == VFS_TYPE_DIR &&
        !(flags & O_DIRECTORY)) {
        if (node->ops->release) node->ops->release(node);
        return VFS_ERR_ISDIR;
    }

    /* O_TRUNC */
    if ((flags & O_TRUNC) && node->stat.type == VFS_TYPE_FILE) {
        if (node->ops->truncate) node->ops->truncate(node, 0);
    }

    return fd_alloc(node, flags);
}

int vfs_close(int fd) {
    vfs_fd_t *f = fd_get(fd);
    if (!f) return VFS_ERR_BADF;
    spin_lock(&g_fd_lock);
    if (f->node && f->node->ops->release)
        f->node->ops->release(f->node);
    kmem_set(f, 0, sizeof(vfs_fd_t));
    spin_unlock(&g_fd_lock);
    return VFS_OK;
}

int64_t vfs_read(int fd, void *buf, uint64_t size) {
    vfs_fd_t *f = fd_get(fd);
    if (!f) return VFS_ERR_BADF;
    if (!f->node->ops->read) return VFS_ERR_UNSUP;
    int64_t r = f->node->ops->read(f->node, f->offset, size, buf);
    if (r > 0) f->offset += (uint64_t)r;
    return r;
}

int64_t vfs_write(int fd, const void *buf, uint64_t size) {
    vfs_fd_t *f = fd_get(fd);
    if (!f) return VFS_ERR_BADF;
    if (f->flags & O_APPEND) f->offset = f->node->stat.size;
    if (!f->node->ops->write) return VFS_ERR_UNSUP;
    int64_t r = f->node->ops->write(f->node, f->offset, size, buf);
    if (r > 0) f->offset += (uint64_t)r;
    return r;
}

int64_t vfs_seek(int fd, int64_t offset, int whence) {
    vfs_fd_t *f = fd_get(fd);
    if (!f) return VFS_ERR_BADF;
    int64_t new_off;
    switch (whence) {
        case 0: new_off = offset; break;
        case 1: new_off = (int64_t)f->offset + offset; break;
        case 2: new_off = (int64_t)f->node->stat.size + offset; break;
        default: return VFS_ERR_INVAL;
    }
    if (new_off < 0) return VFS_ERR_INVAL;
    f->offset = (uint64_t)new_off;
    return new_off;
}

int vfs_stat(const char *path, vfs_stat_t *out) {
    vfs_node_t *node = vfs_resolve(path, false, NULL);
    if (!node) return VFS_ERR_NOENT;
    *out = node->stat;
    if (node->ops->release) node->ops->release(node);
    return VFS_OK;
}

int vfs_fstat(int fd, vfs_stat_t *out) {
    vfs_fd_t *f = fd_get(fd);
    if (!f) return VFS_ERR_BADF;
    *out = f->node->stat;
    return VFS_OK;
}

int vfs_readdir(int fd, uint32_t index, vfs_dirent_t *out) {
    vfs_fd_t *f = fd_get(fd);
    if (!f) return VFS_ERR_BADF;
    if (f->node->stat.type != VFS_TYPE_DIR) return VFS_ERR_NOTDIR;
    if (!f->node->ops->readdir) return VFS_ERR_UNSUP;
    return f->node->ops->readdir(f->node, index, out);
}

int vfs_mkdir(const char *path) {
    char last[VFS_MAX_NAME + 1];
    vfs_node_t *parent = vfs_resolve(path, true, last);
    if (!parent) return VFS_ERR_NOENT;
    if (!parent->ops->create) {
        if (parent->ops->release) parent->ops->release(parent);
        return VFS_ERR_UNSUP;
    }
    vfs_node_t *n = parent->ops->create(parent, last, VFS_TYPE_DIR);
    if (parent->ops->release) parent->ops->release(parent);
    if (!n) return VFS_ERR_IO;
    if (n->ops->release) n->ops->release(n);
    return VFS_OK;
}

int vfs_unlink(const char *path) {
    char last[VFS_MAX_NAME + 1];
    vfs_node_t *parent = vfs_resolve(path, true, last);
    if (!parent) return VFS_ERR_NOENT;
    if (!parent->ops->unlink) {
        if (parent->ops->release) parent->ops->release(parent);
        return VFS_ERR_UNSUP;
    }
    int r = parent->ops->unlink(parent, last);
    if (parent->ops->release) parent->ops->release(parent);
    return r;
}

int vfs_rmdir(const char *path) {
    return vfs_unlink(path);  /* FAT32 unlink проверяет пустоту директории */
}

int vfs_truncate(const char *path, uint64_t size) {
    vfs_node_t *node = vfs_resolve(path, false, NULL);
    if (!node) return VFS_ERR_NOENT;
    int r = VFS_ERR_UNSUP;
    if (node->ops->truncate) r = node->ops->truncate(node, size);
    if (node->ops->release) node->ops->release(node);
    return r;
}

int vfs_ftruncate(int fd, uint64_t size) {
    vfs_fd_t *f = fd_get(fd);
    if (!f) return VFS_ERR_BADF;
    if (!f->node->ops->truncate) return VFS_ERR_UNSUP;
    return f->node->ops->truncate(f->node, size);
}