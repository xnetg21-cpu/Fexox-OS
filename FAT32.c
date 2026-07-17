/*
 * FAT32.c — полная реализация драйвера FAT32 для FEXOS VFS
 *
 * Поддерживается:
 *   - Монтирование FAT12 / FAT16 / FAT32 (автоопределение)
 *   - Длинные имена файлов (LFN, VFAT), UTF-16LE → ASCII/UTF-8
 *   - Чтение и запись файлов (произвольный доступ)
 *   - Создание файлов и директорий
 *   - Удаление файлов и директорий (с очисткой цепочки кластеров)
 *   - Усечение файлов (truncate)
 *   - readdir с LFN
 *   - Кэш FAT (сектора FAT хранятся в памяти при монтировании ≤ 4 МБ FAT)
 *   - Кэш секторов директорий (простой, на один читаемый сектор)
 *   - Корректное обновление FSInfo (free cluster count, next free)
 *   - Атрибуты: READ_ONLY, HIDDEN, SYSTEM, DIRECTORY, ARCHIVE
 *   - Проверка сигнатур BPB и FAT32
 *
 * Ограничения (допустимы для ядра без полного POSIX):
 *   - Нет жёстких / символических ссылок
 *   - Нет прав доступа (FAT не поддерживает)
 *   - Время файлов: только дата создания/изменения (FAT-формат)
 *   - Нет поддержки VFAT > 255 символов в имени
 *   - Без транзакций — сбой в середине записи может испортить ФС
 *
 * Архитектура:
 *   fat32_priv_t  — приватные данные смонтированного тома
 *   fat_node_t    — in-memory представление файла/директории
 *   fat_dir_entry — 32-байтная запись директории на диске
 *   fat_lfn_entry — запись LFN на диске
 */

#include "VFS.h"
#include "debug_out.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Утилиты (без libc)
 * ========================================================================= */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

static inline void *kzalloc(uint64_t size) {
    void *p = kmalloc(size);
    if (p) { uint8_t *b = (uint8_t *)p; for (uint64_t i = 0; i < size; i++) b[i] = 0; }
    return p;
}
static inline void kmem_set(void *d, uint8_t v, uint64_t n) {
    uint8_t *p = (uint8_t *)d; while (n--) *p++ = v;
}
static inline void kmem_cpy(void *d, const void *s, uint64_t n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    while (n--) *dd++ = *ss++;
}
static __attribute__((unused)) inline int kmem_cmp(const void *a, const void *b, uint64_t n) {
    const uint8_t *aa = (const uint8_t *)a, *bb = (const uint8_t *)b;
    while (n--) { if (*aa != *bb) return *aa - *bb; aa++; bb++; }
    return 0;
}
static inline int kstr_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static __attribute__((unused)) inline int kstr_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b;
}
static inline void kstr_cpy(char *d, const char *s, int max) {
    int i = 0; while (i < max - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}

/* Регистронезависимое сравнение (только ASCII) */
static inline char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32); return c;
}
static int kstr_icmp(const char *a, const char *b) {
    while (*a && to_upper(*a) == to_upper(*b)) { a++; b++; }
    return (unsigned char)to_upper(*a) - (unsigned char)to_upper(*b);
}

/* Spinlock UP */
typedef volatile int spinlock_t;
static __attribute__((unused)) inline void spin_lock(spinlock_t *l)   { (void)l; __asm__ volatile("cli":::"memory"); }
static __attribute__((unused)) inline void spin_unlock(spinlock_t *l) { (void)l; __asm__ volatile("sti":::"memory"); }

/* =========================================================================
 * On-disk структуры FAT
 * ========================================================================= */

/* BIOS Parameter Block (BPB) + Extended BPB для FAT32 */
typedef struct __attribute__((packed)) {
    uint8_t  jump[3];           /* 0x00 EB xx 90 или E9 xx xx */
    char     oem_name[8];       /* 0x03 OEM строка */
    uint16_t bytes_per_sector;  /* 0x0B */
    uint8_t  sectors_per_cluster; /* 0x0D */
    uint16_t reserved_sectors;  /* 0x0E */
    uint8_t  num_fats;          /* 0x10 обычно 2 */
    uint16_t root_entry_count;  /* 0x11 для FAT16; 0 для FAT32 */
    uint16_t total_sectors_16;  /* 0x13 */
    uint8_t  media_type;        /* 0x15 */
    uint16_t fat_size_16;       /* 0x16 для FAT16; 0 для FAT32 */
    uint16_t sectors_per_track; /* 0x18 */
    uint16_t num_heads;         /* 0x1A */
    uint32_t hidden_sectors;    /* 0x1C */
    uint32_t total_sectors_32;  /* 0x20 */
    /* Extended BPB FAT32 (только если fat_size_16==0 и root_entry_count==0) */
    uint32_t fat_size_32;       /* 0x24 секторов на одну FAT */
    uint16_t ext_flags;         /* 0x28 */
    uint16_t fs_version;        /* 0x2A должен быть 0x0000 */
    uint32_t root_cluster;      /* 0x2C обычно 2 */
    uint16_t fs_info_sector;    /* 0x30 */
    uint16_t backup_boot_sector;/* 0x32 */
    uint8_t  reserved[12];      /* 0x34 */
    uint8_t  drive_number;      /* 0x40 */
    uint8_t  reserved1;         /* 0x41 */
    uint8_t  boot_signature;    /* 0x42 0x29 */
    uint32_t volume_id;         /* 0x43 */
    char     volume_label[11];  /* 0x47 */
    char     fs_type[8];        /* 0x52 "FAT32   " */
    /* Загрузочный код: 0x5A..0x1FD */
    uint8_t  boot_code[420];
    uint16_t boot_signature2;   /* 0x1FE должно быть 0xAA55 */
} fat_bpb_t;

/* FSInfo (сектор fs_info_sector) */
typedef struct __attribute__((packed)) {
    uint32_t lead_sig;      /* 0x41615252 */
    uint8_t  reserved1[480];
    uint32_t struct_sig;    /* 0x61417272 */
    uint32_t free_count;    /* 0xFFFFFFFF = неизвестно */
    uint32_t next_free;     /* hint для поиска свободного кластера */
    uint8_t  reserved2[12];
    uint32_t trail_sig;     /* 0xAA550000 */
} fat_fsinfo_t;

#define FSINFO_LEAD_SIG   0x41615252UL
#define FSINFO_STRUCT_SIG 0x61417272UL
#define FSINFO_TRAIL_SIG  0xAA550000UL

/* 32-байтная запись директории */
typedef struct __attribute__((packed)) {
    uint8_t  name[11];         /* 8.3 имя в верхнем регистре */
    uint8_t  attr;             /* атрибуты */
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi; /* старшие 16 бит первого кластера */
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo; /* младшие 16 бит */
    uint32_t file_size;
} fat_dir_entry_t;

/* LFN запись (тоже 32 байта) */
typedef struct __attribute__((packed)) {
    uint8_t  order;        /* порядковый номер | 0x40 для последнего */
    uint16_t name1[5];     /* символы 1-5  (UTF-16LE) */
    uint8_t  attr;         /* 0x0F (ATTR_LFN) */
    uint8_t  type;         /* 0x00 */
    uint8_t  checksum;
    uint16_t name2[6];     /* символы 6-11 */
    uint16_t cluster;      /* 0x0000 */
    uint16_t name3[2];     /* символы 12-13 */
} fat_lfn_entry_t;

/* Атрибуты */
#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)
#define ATTR_LFN_MASK   0x3F

#define FAT_ENTRY_FREE      0xE5
#define FAT_ENTRY_END       0x00
#define FAT_ENTRY_DOT       '.'

/* Значения FAT */
#define FAT32_EOC       0x0FFFFFF8UL  /* End Of Chain */
#define FAT32_BAD       0x0FFFFFF7UL
#define FAT32_FREE      0x00000000UL
#define FAT32_MASK      0x0FFFFFFFUL  /* 28 бит */

/* =========================================================================
 * Приватные данные тома
 * ========================================================================= */
#define FAT_TYPE_12  12
#define FAT_TYPE_16  16
#define FAT_TYPE_32  32

/* Максимальный размер кэша FAT (4 МБ) */
#define FAT_CACHE_MAX  (4 * 1024 * 1024)

typedef struct {
    blkdev_t *dev;
    spinlock_t lock;

    /* Параметры BPB */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t fat_size;           /* секторов на одну FAT */
    uint32_t root_cluster;       /* первый кластер корневой директории */
    uint32_t total_clusters;
    uint32_t data_start_sector;  /* первый сектор области данных */
    uint32_t first_fat_sector;   /* первый сектор первой FAT */
    uint16_t fs_info_sector;
    int      fat_type;           /* 12, 16, 32 */

    uint32_t bytes_per_cluster;
    uint32_t sectors_per_cluster32; /* uint32_t версия */

    /* Кэш FAT */
    uint8_t  *fat_cache;         /* NULL если нет кэша */
    uint32_t  fat_cache_sectors; /* сколько секторов закэшировано */
    bool      fat_cache_dirty;

    /* Свободные кластеры (из FSInfo) */
    uint32_t free_cluster_count;
    uint32_t next_free_cluster;

    /* Вспомогательный буфер (один сектор) */
    uint8_t  *sector_buf;
    uint32_t  sector_buf_lba;    /* -1 если не закэширован */
    bool      sector_buf_dirty;

    bool     readonly;
} fat32_priv_t;

/* =========================================================================
 * In-memory узел FAT
 * ========================================================================= */
typedef struct {
    fat32_priv_t *fat;           /* ссылка на том */
    uint32_t      first_cluster; /* первый кластер (0 = пусто/корень) */
    uint64_t      size;          /* размер в байтах */
    uint8_t       attr;          /* FAT атрибуты */
    bool          dirty;         /* нужно записать? */

    /* Расположение записи директории на диске (для обновления) */
    uint32_t dir_cluster;        /* кластер директории-родителя */
    uint32_t dir_entry_idx;      /* индекс записи в директории */
    bool     is_root;            /* корневая директория */
} fat_node_t;

/* =========================================================================
 * Вспомогательные функции — работа с диском
 * ========================================================================= */

static uint64_t cluster_to_lba(fat32_priv_t *fat, uint32_t cluster) {
    return fat->data_start_sector
         + (uint64_t)(cluster - 2) * fat->sectors_per_cluster32;
}

/* Чтение одного сектора (через кэш sector_buf) */
static __attribute__((unused)) int fat_read_sector(fat32_priv_t *fat, uint32_t lba, void *buf) {
    if (fat->sector_buf_lba == lba) {
        kmem_cpy(buf, fat->sector_buf, fat->bytes_per_sector);
        return VFS_OK;
    }
    /* Сбрасываем грязный буфер */
    if (fat->sector_buf_dirty && fat->sector_buf_lba != (uint32_t)-1) {
        blkdev_write(fat->dev, fat->sector_buf_lba, 1, fat->sector_buf);
        fat->sector_buf_dirty = false;
    }
    int r = blkdev_read(fat->dev, lba, 1, fat->sector_buf);
    if (r != VFS_OK) { fat->sector_buf_lba = (uint32_t)-1; return r; }
    fat->sector_buf_lba = lba;
    kmem_cpy(buf, fat->sector_buf, fat->bytes_per_sector);
    return VFS_OK;
}

/* Запись одного сектора */
static __attribute__((unused)) int fat_write_sector(fat32_priv_t *fat, uint32_t lba, const void *buf) {
    if (fat->readonly) return VFS_ERR_ROFS;
    /* Обновляем кэш если это тот же сектор */
    if (fat->sector_buf_lba == lba) {
        kmem_cpy(fat->sector_buf, buf, fat->bytes_per_sector);
        fat->sector_buf_dirty = false; /* пишем сразу */
    }
    return blkdev_write(fat->dev, lba, 1, buf);
}

/* Чтение кластера данных (возвращает указатель внутри выделенного буфера) */
static int fat_read_cluster(fat32_priv_t *fat, uint32_t cluster, void *buf) {
    uint64_t lba = cluster_to_lba(fat, cluster);
    DBG_VAL("FAT", "read_cluster cluster", (uint64_t)cluster);
    DBG_VAL("FAT", "read_cluster lba",     lba);
    int _r = blkdev_read(fat->dev, lba, fat->sectors_per_cluster32, buf);
    if (_r != VFS_OK) DBG_VAL("FAT", "read_cluster FAILED", (uint64_t)(int64_t)_r);
    return _r;
}

static int fat_write_cluster(fat32_priv_t *fat, uint32_t cluster, const void *buf) {
    if (fat->readonly) return VFS_ERR_ROFS;
    uint64_t lba = cluster_to_lba(fat, cluster);
    return blkdev_write(fat->dev, lba, fat->sectors_per_cluster32, buf);
}

/* =========================================================================
 * FAT таблица: чтение и запись записей
 * ========================================================================= */

static uint32_t fat_get_entry(fat32_priv_t *fat, uint32_t cluster) {
    if (cluster < 2 || cluster >= fat->total_clusters + 2) return FAT32_EOC;

    if (fat->fat_type == FAT_TYPE_32) {
        if (fat->fat_cache) {
            uint32_t *table = (uint32_t *)fat->fat_cache;
            return table[cluster] & FAT32_MASK;
        }
        /* Без кэша: читаем нужный сектор */
        uint32_t offset = cluster * 4;
        uint32_t sector = fat->first_fat_sector + offset / fat->bytes_per_sector;
        uint32_t off_in = offset % fat->bytes_per_sector;
        uint8_t  tmp[512];
        if (blkdev_read(fat->dev, sector, 1, tmp) != VFS_OK) return FAT32_EOC;
        uint32_t val;
        kmem_cpy(&val, tmp + off_in, 4);
        return val & FAT32_MASK;
    }

    if (fat->fat_type == FAT_TYPE_16) {
        if (fat->fat_cache) {
            uint16_t *table = (uint16_t *)fat->fat_cache;
            uint32_t v = table[cluster];
            if (v >= 0xFFF8) return FAT32_EOC;
            if (v == 0)      return FAT32_FREE;
            return v;
        }
        uint32_t offset = cluster * 2;
        uint32_t sector = fat->first_fat_sector + offset / fat->bytes_per_sector;
        uint32_t off_in = offset % fat->bytes_per_sector;
        uint8_t  tmp[512];
        if (blkdev_read(fat->dev, sector, 1, tmp) != VFS_OK) return FAT32_EOC;
        uint16_t val; kmem_cpy(&val, tmp + off_in, 2);
        if (val >= 0xFFF8) return FAT32_EOC;
        if (val == 0)      return FAT32_FREE;
        return val;
    }

    /* FAT12 */
    uint32_t offset = cluster + (cluster / 2);
    uint32_t sector = fat->first_fat_sector + offset / fat->bytes_per_sector;
    uint32_t off_in = offset % fat->bytes_per_sector;
    uint8_t  tmp[512 * 2];
    if (blkdev_read(fat->dev, sector, 2, tmp) != VFS_OK) return FAT32_EOC;
    uint16_t val; kmem_cpy(&val, tmp + off_in, 2);
    if (cluster & 1) val >>= 4; else val &= 0x0FFF;
    if (val >= 0xFF8) return FAT32_EOC;
    if (val == 0)     return FAT32_FREE;
    return val;
}

static int fat_set_entry(fat32_priv_t *fat, uint32_t cluster, uint32_t value) {
    if (fat->readonly) return VFS_ERR_ROFS;

    if (fat->fat_type == FAT_TYPE_32) {
        if (fat->fat_cache) {
            uint32_t *table = (uint32_t *)fat->fat_cache;
            table[cluster] = (table[cluster] & ~FAT32_MASK) | (value & FAT32_MASK);
            fat->fat_cache_dirty = true;
            return VFS_OK;
        }
        uint32_t offset = cluster * 4;
        uint32_t sector = fat->first_fat_sector + offset / fat->bytes_per_sector;
        uint32_t off_in = offset % fat->bytes_per_sector;
        uint8_t  tmp[512];
        if (blkdev_read(fat->dev, sector, 1, tmp) != VFS_OK) return VFS_ERR_IO;
        uint32_t existing; kmem_cpy(&existing, tmp + off_in, 4);
        existing = (existing & ~FAT32_MASK) | (value & FAT32_MASK);
        kmem_cpy(tmp + off_in, &existing, 4);
        return blkdev_write(fat->dev, sector, 1, tmp) == VFS_OK ? VFS_OK : VFS_ERR_IO;
    }

    if (fat->fat_type == FAT_TYPE_16) {
        uint16_t v16 = (value >= FAT32_EOC) ? 0xFFFF :
                       (value == FAT32_FREE) ? 0x0000 : (uint16_t)(value & 0xFFFF);
        if (fat->fat_cache) {
            uint16_t *table = (uint16_t *)fat->fat_cache;
            table[cluster] = v16;
            fat->fat_cache_dirty = true;
            return VFS_OK;
        }
        uint32_t offset = cluster * 2;
        uint32_t sector = fat->first_fat_sector + offset / fat->bytes_per_sector;
        uint32_t off_in = offset % fat->bytes_per_sector;
        uint8_t  tmp[512];
        if (blkdev_read(fat->dev, sector, 1, tmp) != VFS_OK) return VFS_ERR_IO;
        kmem_cpy(tmp + off_in, &v16, 2);
        return blkdev_write(fat->dev, sector, 1, tmp) == VFS_OK ? VFS_OK : VFS_ERR_IO;
    }

    /* FAT12 */
    uint32_t offset = cluster + (cluster / 2);
    uint32_t sector = fat->first_fat_sector + offset / fat->bytes_per_sector;
    uint32_t off_in = offset % fat->bytes_per_sector;
    uint8_t  tmp[512 * 2];
    if (blkdev_read(fat->dev, sector, 2, tmp) != VFS_OK) return VFS_ERR_IO;
    uint16_t existing; kmem_cpy(&existing, tmp + off_in, 2);
    uint16_t v12 = (value >= FAT32_EOC) ? 0xFF8 :
                   (value == FAT32_FREE) ? 0x000 : (uint16_t)(value & 0xFFF);
    if (cluster & 1) existing = (uint16_t)((existing & 0x000F) | (v12 << 4));
    else             existing = (uint16_t)((existing & 0xF000) | (v12 & 0xFFF));
    kmem_cpy(tmp + off_in, &existing, 2);
    return blkdev_write(fat->dev, sector, 2, tmp) == VFS_OK ? VFS_OK : VFS_ERR_IO;
}

/* Сброс кэша FAT на диск (обе копии FAT) */
static int fat_flush_fat(fat32_priv_t *fat) {
    if (!fat->fat_cache_dirty) return VFS_OK;
    int r = blkdev_write(fat->dev, fat->first_fat_sector,
                         fat->fat_cache_sectors, fat->fat_cache);
    if (r != VFS_OK) return r;
    /* Вторая копия FAT */
    if (fat->num_fats >= 2) {
        r = blkdev_write(fat->dev,
                         fat->first_fat_sector + fat->fat_size,
                         fat->fat_cache_sectors, fat->fat_cache);
    }
    fat->fat_cache_dirty = false;
    return r;
}

/* =========================================================================
 * Операции с кластерами: цепочка, выделение, освобождение
 * ========================================================================= */

/* Получить N-й кластер цепочки (0-based) */
static __attribute__((unused)) uint32_t fat_follow_chain(fat32_priv_t *fat, uint32_t start, uint32_t n) {
    uint32_t cur = start;
    for (uint32_t i = 0; i < n; i++) {
        cur = fat_get_entry(fat, cur);
        if (cur >= FAT32_EOC || cur == FAT32_FREE || cur == FAT32_BAD)
            return 0;
    }
    return cur;
}

/* Выделить один свободный кластер (и добавить EOC) */
static uint32_t fat_alloc_cluster(fat32_priv_t *fat) {
    uint32_t start = fat->next_free_cluster;
    if (start < 2) start = 2;

    for (uint32_t i = 0; i < fat->total_clusters; i++) {
        uint32_t c = 2 + ((start - 2 + i) % fat->total_clusters);
        if (fat_get_entry(fat, c) == FAT32_FREE) {
            fat_set_entry(fat, c, FAT32_EOC);
            fat->next_free_cluster = c + 1;
            if (fat->free_cluster_count != 0xFFFFFFFF && fat->free_cluster_count > 0)
                fat->free_cluster_count--;
            return c;
        }
    }
    return 0; /* нет свободных */
}

/* Освободить всю цепочку кластеров начиная с start */
static int fat_free_chain(fat32_priv_t *fat, uint32_t start) {
    uint32_t cur = start;
    while (cur >= 2 && cur < FAT32_EOC) {
        uint32_t next = fat_get_entry(fat, cur);
        int r = fat_set_entry(fat, cur, FAT32_FREE);
        if (r != VFS_OK) return r;
        if (fat->free_cluster_count != 0xFFFFFFFF)
            fat->free_cluster_count++;
        if (cur < fat->next_free_cluster)
            fat->next_free_cluster = cur;
        cur = next;
    }
    return VFS_OK;
}

/* Добавить новый кластер в конец цепочки */
static uint32_t fat_extend_chain(fat32_priv_t *fat, uint32_t last_cluster) {
    uint32_t new_c = fat_alloc_cluster(fat);
    if (new_c == 0) return 0;
    if (last_cluster >= 2 && last_cluster < FAT32_EOC)
        fat_set_entry(fat, last_cluster, new_c);
    return new_c;
}

/* Длина цепочки в кластерах */
static uint32_t fat_chain_length(fat32_priv_t *fat, uint32_t start) {
    uint32_t cur = start, n = 0;
    while (cur >= 2 && cur < FAT32_EOC && cur != FAT32_FREE) {
        cur = fat_get_entry(fat, cur);
        n++;
        if (n > fat->total_clusters) break; /* защита от петли */
    }
    return n;
}

/* =========================================================================
 * FSInfo — обновление
 * ========================================================================= */
static void fat_write_fsinfo(fat32_priv_t *fat) {
    if (fat->readonly || fat->fat_type != FAT_TYPE_32) return;
    if (fat->fs_info_sector == 0 || fat->fs_info_sector == 0xFFFF) return;

    uint8_t buf[512];
    if (blkdev_read(fat->dev, fat->fs_info_sector, 1, buf) != VFS_OK) return;

    fat_fsinfo_t *fi = (fat_fsinfo_t *)buf;
    if (fi->lead_sig   != FSINFO_LEAD_SIG  ||
        fi->struct_sig != FSINFO_STRUCT_SIG ||
        fi->trail_sig  != FSINFO_TRAIL_SIG) return;

    fi->free_count = fat->free_cluster_count;
    fi->next_free  = fat->next_free_cluster;
    blkdev_write(fat->dev, fat->fs_info_sector, 1, buf);
}

/* =========================================================================
 * Работа с именами
 * ========================================================================= */

/* UTF-16LE → ASCII/Latin-1 (простое преобразование для имён файлов) */
static void utf16le_to_ascii(const uint16_t *src, int count, char *dst, int dst_max) {
    int di = 0;
    for (int i = 0; i < count && di < dst_max - 1; i++) {
        uint16_t c = src[i];
        if (c == 0x0000 || c == 0xFFFF) break;
        if (c < 0x80) dst[di++] = (char)(c & 0x7F);
        else if (c < 0x800) {
            if (di + 2 >= dst_max) break;
            dst[di++] = (char)(0xC0 | (c >> 6));
            dst[di++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (di + 3 >= dst_max) break;
            dst[di++] = (char)(0xE0 | (c >> 12));
            dst[di++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[di] = '\0';
}

/* ASCII/UTF-8 → UTF-16LE */
static void ascii_to_utf16le(const char *src, uint16_t *dst, int count) {
    for (int i = 0; i < count; i++) {
        dst[i] = (src[i]) ? (uint16_t)(unsigned char)src[i] : 0x0000;
    }
}

/* Расшифровать 8.3 имя в строку (без пробелов) */
static void fat_parse_83name(const uint8_t *raw, char *out) {
    int oi = 0;
    /* Имя (8 символов) */
    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[oi++] = (char)raw[i];
    /* Расширение (3 символа) */
    if (raw[8] != ' ') {
        out[oi++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[oi++] = (char)raw[i];
    }
    out[oi] = '\0';
}

/* Закодировать имя в 8.3 формат (возвращает false если не вмещается) */
static bool fat_make_83name(const char *name, uint8_t *raw) {
    kmem_set(raw, ' ', 11);
    int ni = 0;
    int len = kstr_len(name);

    /* Ищем последнюю точку для разделения имя.расширение */
    int dot = -1;
    for (int i = 0; i < len; i++) if (name[i] == '.') dot = i;

    int base_len = (dot >= 0) ? dot : len;
    int ext_len  = (dot >= 0) ? (len - dot - 1) : 0;

    if (base_len > 8 || ext_len > 3) return false;

    for (int i = 0; i < base_len; i++)
        raw[ni++] = (uint8_t)to_upper(name[i]);
    ni = 8;
    for (int i = 0; i < ext_len; i++)
        raw[ni++] = (uint8_t)to_upper(name[dot + 1 + i]);
    return true;
}

/* LFN чексумма по 8.3 имени */
static uint8_t fat_lfn_checksum(const uint8_t *name83) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)((sum >> 1) | (sum << 7)) + name83[i];
    return sum;
}

/* =========================================================================
 * Операции с директориями
 * ========================================================================= */

/* Количество секторов в директории (FAT: одна цепочка кластеров) */
/* Для корневой FAT16/12 — фиксированная область */

typedef struct {
    fat32_priv_t *fat;
    uint32_t      cluster;   /* текущий кластер (0 = корень FAT16/12) */
    uint32_t      sector_in_cluster;
    uint32_t      entry_in_sector;
    uint32_t      global_entry_idx; /* абсолютный номер записи */
    bool          is_root_fat16;
    uint32_t      root_sector_count;
    uint8_t       sector_buf[4096]; /* буфер до 8 секторов/кластер */
    bool          buf_valid;
    uint32_t      buf_cluster;
} fat_dir_iter_t;

static void fat_dir_iter_init(fat_dir_iter_t *it, fat32_priv_t *fat, uint32_t cluster) {
    it->fat = fat;
    it->cluster = cluster;
    it->sector_in_cluster = 0;
    it->entry_in_sector = 0;
    it->global_entry_idx = 0;
    it->buf_valid = false;
    it->buf_cluster = 0;

    /* Корень FAT16/12 */
    it->is_root_fat16 = (fat->fat_type != FAT_TYPE_32 && cluster == 0);
    if (it->is_root_fat16) {
        /* Корень: reserved_sectors + num_fats * fat_size */
        it->root_sector_count = 0; /* вычислим из root_entry_count */
        /* root_entry_count хранится отдельно — передаётся через cluster=0 */
        it->cluster = 0;
    }
}

/* Чтение следующей записи директории */
/* Возвращает VFS_OK (запись прочитана), VFS_ERR_NOENT (конец), или ошибку */
static int fat_dir_read_entry(fat_dir_iter_t *it, fat_dir_entry_t *entry,
                               uint32_t *out_lba, uint32_t *out_entry_in_sector) {
    fat32_priv_t *fat = it->fat;
    uint32_t eps = fat->bytes_per_sector / sizeof(fat_dir_entry_t); /* записей в секторе */

    while (1) {
        /* Загружаем буфер кластера если надо */
        if (!it->buf_valid || it->buf_cluster != it->cluster) {
            if (it->is_root_fat16) {
                /* FAT12/16: корень — фиксированная зона сразу после FAT */
                uint32_t root_start = fat->first_fat_sector
                                    + fat->num_fats * fat->fat_size;
                uint32_t lba = root_start + it->sector_in_cluster;
                if (blkdev_read(fat->dev, lba, 1, it->sector_buf) != VFS_OK)
                    return VFS_ERR_IO;
            } else {
                if (it->cluster < 2) return VFS_ERR_NOENT;
                uint64_t lba = cluster_to_lba(fat, it->cluster);
                if (blkdev_read(fat->dev, lba,
                                fat->sectors_per_cluster32,
                                it->sector_buf) != VFS_OK)
                    return VFS_ERR_IO;
            }
            it->buf_valid  = true;
            it->buf_cluster = it->cluster;
        }

        /* Текущая запись */
        uint32_t offset = it->sector_in_cluster * fat->bytes_per_sector
                        + it->entry_in_sector * sizeof(fat_dir_entry_t);
        fat_dir_entry_t *e = (fat_dir_entry_t *)(it->sector_buf + offset);

        uint32_t lba;
        if (it->is_root_fat16) {
            uint32_t root_start = fat->first_fat_sector + fat->num_fats * fat->fat_size;
            lba = root_start + it->sector_in_cluster;
        } else {
            lba = (uint32_t)cluster_to_lba(fat, it->cluster) + it->sector_in_cluster;
        }
        uint32_t eis = it->entry_in_sector;

        /* Конец директории */
        if (e->name[0] == FAT_ENTRY_END) return VFS_ERR_NOENT;

        /* Обновляем позицию */
        it->entry_in_sector++;
        it->global_entry_idx++;
        if (it->entry_in_sector >= eps) {
            it->entry_in_sector = 0;
            it->sector_in_cluster++;
            it->buf_valid = false;

            uint32_t spc = it->is_root_fat16 ? 1 : fat->sectors_per_cluster32;
            if (it->sector_in_cluster >= spc) {
                it->sector_in_cluster = 0;
                if (it->is_root_fat16) {
                    /* Конец корневой зоны FAT16 */
                    return VFS_ERR_NOENT;
                }
                uint32_t next = fat_get_entry(fat, it->cluster);
                if (next >= FAT32_EOC || next == FAT32_FREE)
                    return VFS_ERR_NOENT;
                it->cluster = next;
            }
        }

        kmem_cpy(entry, e, sizeof(fat_dir_entry_t));
        if (out_lba) *out_lba = lba;
        if (out_entry_in_sector) *out_entry_in_sector = eis;
        return VFS_OK;
    }
}

/* =========================================================================
 * Поиск файла в директории
 * Заполняет entry и возвращает позицию записи (lba + entry_in_sector)
 * При наличии LFN — возвращает настоящее имя в lfn_name
 * ========================================================================= */
typedef struct {
    fat_dir_entry_t entry;
    char            long_name[VFS_MAX_NAME + 1];
    char            short_name[16];
    uint32_t        entry_lba;        /* lba сектора с основной записью */
    uint32_t        entry_in_sector;  /* индекс в секторе */
    uint32_t        lfn_start_lba;    /* lba первой LFN записи */
    uint32_t        lfn_start_eis;    /* entry_in_sector первой LFN */
    uint32_t        entry_cluster;    /* кластер директории */
    uint32_t        entry_global_idx; /* глобальный индекс основной записи */
} fat_found_t;

/* Итератор с LFN буфером */
#define LFN_BUF_MAX 20  /* максимум LFN записей (20 * 13 = 260 символов) */

static int fat_dir_find(fat32_priv_t *fat, uint32_t dir_cluster,
                        const char *name, fat_found_t *out) {
    fat_dir_iter_t it;
    fat_dir_iter_init(&it, fat, dir_cluster);

    /* LFN буфер: собираем фрагменты */
    uint16_t lfn_buf[LFN_BUF_MAX * 13 + 1];
    uint8_t  lfn_checksum = 0;
    bool     has_lfn      = false;
    uint32_t lfn_first_lba = 0, lfn_first_eis = 0;

    fat_dir_entry_t entry;
    uint32_t lba, eis;

    while (fat_dir_read_entry(&it, &entry, &lba, &eis) == VFS_OK) {
        uint8_t first = entry.name[0];

        /* Удалённая запись — сбрасываем LFN */
        if (first == FAT_ENTRY_FREE) { has_lfn = false; continue; }

        /* LFN запись */
        if ((entry.attr & ATTR_LFN_MASK) == ATTR_LFN) {
            fat_lfn_entry_t *lfn = (fat_lfn_entry_t *)&entry;
            int seq = lfn->order & 0x1F;
            bool is_last = !!(lfn->order & 0x40);

            if (is_last) {
                /* Начало нового LFN (записи идут в обратном порядке) */
                kmem_set(lfn_buf, 0, sizeof(lfn_buf));
                lfn_checksum  = lfn->checksum;
                has_lfn       = true;
                lfn_first_lba = lba;
                lfn_first_eis = eis;
            } else if (!has_lfn || lfn->checksum != lfn_checksum) {
                has_lfn = false; continue;
            }

            /* Копируем 13 символов в нужную позицию буфера */
            int pos = (seq - 1) * 13;
            if (pos + 13 <= LFN_BUF_MAX * 13) {
                kmem_cpy(lfn_buf + pos,      lfn->name1, 5 * 2);
                kmem_cpy(lfn_buf + pos + 5,  lfn->name2, 6 * 2);
                kmem_cpy(lfn_buf + pos + 11, lfn->name3, 2 * 2);
            }
            continue;
        }

        /* Volume label — пропускаем */
        if (entry.attr & ATTR_VOLUME_ID) { has_lfn = false; continue; }

        /* Нормальная запись */
        char short_name[16];
        fat_parse_83name(entry.name, short_name);

        char long_name[VFS_MAX_NAME + 1];
        long_name[0] = '\0';
        if (has_lfn && fat_lfn_checksum(entry.name) == lfn_checksum) {
            utf16le_to_ascii(lfn_buf, LFN_BUF_MAX * 13, long_name, VFS_MAX_NAME + 1);
        }

        /* Сравниваем */
        const char *cmp_name = (long_name[0] != '\0') ? long_name : short_name;
        if (kstr_icmp(cmp_name, name) == 0 ||
            (long_name[0] != '\0' && kstr_icmp(short_name, name) == 0)) {
            if (out) {
                kmem_cpy(&out->entry, &entry, sizeof(fat_dir_entry_t));
                kstr_cpy(out->long_name, long_name[0] ? long_name : short_name,
                          VFS_MAX_NAME + 1);
                kstr_cpy(out->short_name, short_name, 16);
                out->entry_lba        = lba;
                out->entry_in_sector  = eis;
                out->lfn_start_lba    = has_lfn ? lfn_first_lba : lba;
                out->lfn_start_eis    = has_lfn ? lfn_first_eis : eis;
                out->entry_cluster    = dir_cluster;
                out->entry_global_idx = it.global_entry_idx - 1;
            }
            return VFS_OK;
        }

        has_lfn = false;
    }

    return VFS_ERR_NOENT;
}

/* Чтение N-й видимой записи директории (для readdir) */
static int fat_dir_read_nth(fat32_priv_t *fat, uint32_t dir_cluster,
                             uint32_t index, vfs_dirent_t *out) {
    fat_dir_iter_t it;
    fat_dir_iter_init(&it, fat, dir_cluster);

    uint16_t lfn_buf[LFN_BUF_MAX * 13 + 1];
    uint8_t  lfn_checksum = 0;
    bool     has_lfn      = false;

    fat_dir_entry_t entry;
    uint32_t lba, eis;
    uint32_t visible_idx = 0;

    while (fat_dir_read_entry(&it, &entry, &lba, &eis) == VFS_OK) {
        uint8_t first = entry.name[0];
        if (first == FAT_ENTRY_FREE) { has_lfn = false; continue; }

        if ((entry.attr & ATTR_LFN_MASK) == ATTR_LFN) {
            fat_lfn_entry_t *lfn = (fat_lfn_entry_t *)&entry;
            int seq = lfn->order & 0x1F;
            bool is_last = !!(lfn->order & 0x40);
            if (is_last) {
                kmem_set(lfn_buf, 0, sizeof(lfn_buf));
                lfn_checksum = lfn->checksum;
                has_lfn = true;
            } else if (!has_lfn || lfn->checksum != lfn_checksum) {
                has_lfn = false; continue;
            }
            int pos = (seq - 1) * 13;
            if (pos + 13 <= LFN_BUF_MAX * 13) {
                kmem_cpy(lfn_buf + pos,      lfn->name1, 10);
                kmem_cpy(lfn_buf + pos + 5,  lfn->name2, 12);
                kmem_cpy(lfn_buf + pos + 11, lfn->name3, 4);
            }
            continue;
        }

        if (entry.attr & ATTR_VOLUME_ID) { has_lfn = false; continue; }

        /* Пропускаем "." и ".." — как и LFN-продолжения/volume-id выше,
         * НЕ считаем их видимой позицией. Раньше здесь стоял
         * visible_idx++, из-за чего в любой НЕ-корневой директории
         * (у корня FAT32 записей "."/".." физически нет, а у всех
         * остальных — всегда есть, это первые две записи) первый
         * настоящий файл оказывался под visible_idx=2, а не 0. Вызывающий
         * код (fm_reload_dir/vfs_readdir) всегда начинает с index=0,
         * поэтому совпадения не находилось никогда — readdir() любой
         * подпапки выглядел как VFS_ERR_NOENT сразу на первом вызове,
         * хотя fat_dir_find() (поиск по имени, отдельная функция) те же
         * файлы прекрасно находил. */
        if (entry.name[0] == '.' && (entry.name[1] == ' ' || entry.name[1] == '.')) {
            has_lfn = false;
            continue;
        }

        if (visible_idx == index) {
            char short_name[16];
            fat_parse_83name(entry.name, short_name);
            char long_name[VFS_MAX_NAME + 1];
            long_name[0] = '\0';
            if (has_lfn && fat_lfn_checksum(entry.name) == lfn_checksum)
                utf16le_to_ascii(lfn_buf, LFN_BUF_MAX * 13, long_name, VFS_MAX_NAME + 1);

            const char *real_name = (long_name[0] != '\0') ? long_name : short_name;
            kstr_cpy(out->name, real_name, VFS_MAX_NAME + 1);
            out->type    = (entry.attr & ATTR_DIRECTORY) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            out->size    = entry.file_size;
            out->cluster = ((uint32_t)entry.first_cluster_hi << 16)
                         | entry.first_cluster_lo;
            return VFS_OK;
        }

        visible_idx++;
        has_lfn = false;
    }

    return VFS_ERR_NOENT;
}

/* =========================================================================
 * Запись в директорию: создание новой записи (с LFN)
 * ========================================================================= */

/* Подсчёт нужных LFN записей */
static int fat_lfn_entries_needed(const char *name) {
    int len = kstr_len(name);
    return (len + 12) / 13;
}

/* Является ли имя чистым 8.3 */
static __attribute__((unused)) bool fat_is_valid_83(const char *name) {
    uint8_t tmp[11];
    return fat_make_83name(name, tmp);
}

/* Записать новую запись в директорию.
   Если нет 8.3 имени, генерирует "shortcut" вида NAMEXX~N.EXT */
static int fat_dir_add_entry(fat32_priv_t *fat, uint32_t dir_cluster,
                              const char *name, uint8_t attr,
                              uint32_t first_cluster, uint32_t file_size,
                              uint32_t *out_entry_lba, uint32_t *out_entry_eis) {
    if (fat->readonly) return VFS_ERR_ROFS;

    /* Формируем 8.3 имя */
    uint8_t name83[11];
    bool use_lfn = !fat_make_83name(name, name83);
    int lfn_count = use_lfn ? fat_lfn_entries_needed(name) : 0;
    int total_entries = lfn_count + 1;

    /* Если нужен LFN но имя влезает в 8.3 с символами — всё равно пишем LFN
       для хранения точного регистра и специальных символов */
    if (!use_lfn && (kstr_len(name) > 0)) {
        /* Проверяем нужен ли LFN для сохранения регистра */
        char check[16];
        fat_parse_83name(name83, check);
        if (kstr_icmp(check, name) != 0) {
            use_lfn = true;
            lfn_count = fat_lfn_entries_needed(name);
            total_entries = lfn_count + 1;
        }
    }

    uint8_t checksum = fat_lfn_checksum(name83);

    /* Ищем total_entries последовательных свободных слотов */
    /* Сначала сканируем существующие кластеры */
    fat_dir_iter_t it;
    fat_dir_iter_init(&it, fat, dir_cluster);

    /* Нам нужно найти N подряд свободных/удалённых записей.
       Для простоты: ищем позицию первой свободной и проверяем N подряд. */

    /* Собираем LBA/eis всех записей пока не найдём свободный блок */
    /* Упрощение: читаем по одной, считаем подряд идущие */

    uint32_t free_run_start_lba = 0;
    uint32_t free_run_start_eis = 0;
    uint32_t free_run = 0;
    bool found_slot = false;

    fat_dir_entry_t entry;
    uint32_t lba, eis;

    /* Сначала сбрасываем итератор */
    fat_dir_iter_init(&it, fat, dir_cluster);
    free_run = 0;

    while (1) {
        int r = fat_dir_read_entry(&it, &entry, &lba, &eis);
        if (r == VFS_ERR_NOENT) {
            /* Конец директории — нужно расширить */
            break;
        }
        if (r != VFS_OK) return r;

        if (entry.name[0] == FAT_ENTRY_FREE || entry.name[0] == FAT_ENTRY_END) {
            if (free_run == 0) {
                free_run_start_lba = lba;
                free_run_start_eis = eis;
            }
            free_run++;
            if ((int)free_run >= total_entries) {
                found_slot = true;
                break;
            }
            if (entry.name[0] == FAT_ENTRY_END) break;
        } else {
            free_run = 0;
        }
    }

    /* Если не нашли — расширяем директорию */
    if (!found_slot) {
        /* Находим последний кластер директории */
        uint32_t last_c = dir_cluster;
        uint32_t cur = dir_cluster;
        while (cur >= 2 && cur < FAT32_EOC) {
            last_c = cur;
            cur = fat_get_entry(fat, cur);
        }
        /* Добавляем новый кластер */
        uint32_t new_c = fat_extend_chain(fat, last_c);
        if (new_c == 0) return VFS_ERR_NOSPC;

        /* Заполняем кластер нулями */
        uint8_t *zero = (uint8_t *)kzalloc(fat->bytes_per_cluster);
        if (!zero) { fat_set_entry(fat, new_c, FAT32_FREE); return VFS_ERR_NOMEM; }
        fat_write_cluster(fat, new_c, zero);
        kfree(zero);

        free_run_start_lba = (uint32_t)cluster_to_lba(fat, new_c);
        free_run_start_eis = 0;
    }

    /* Записываем LFN и основную запись */
    uint32_t eps = fat->bytes_per_sector / sizeof(fat_dir_entry_t);

    /* Позиция для записи (начинаем с free_run_start) */
    uint32_t cur_lba = free_run_start_lba;
    uint32_t cur_eis = free_run_start_eis;

    uint8_t sector_tmp[512];

    /* Функция записи одной записи в директорию */
    #define WRITE_DIRENTRY(e_ptr) do { \
        if (blkdev_read(fat->dev, cur_lba, 1, sector_tmp) != VFS_OK) return VFS_ERR_IO; \
        kmem_cpy(sector_tmp + cur_eis * sizeof(fat_dir_entry_t), (e_ptr), sizeof(fat_dir_entry_t)); \
        if (blkdev_write(fat->dev, cur_lba, 1, sector_tmp) != VFS_OK) return VFS_ERR_IO; \
        cur_eis++; \
        if (cur_eis >= eps) { cur_eis = 0; cur_lba++; /* упрощение: в одном кластере */ } \
    } while(0)

    /* Пишем LFN записи (от последней к первой по порядку) */
    int name_len = kstr_len(name);
    for (int li = lfn_count; li >= 1; li--) {
        fat_lfn_entry_t lfn;
        kmem_set(&lfn, 0, sizeof(lfn));

        int pos = (li - 1) * 13;
        char seg[14];
        for (int ci = 0; ci < 13; ci++) {
            seg[ci] = (pos + ci < name_len) ? name[pos + ci] : 0;
        }
        seg[13] = 0;

        lfn.order    = (uint8_t)li | (li == lfn_count ? 0x40 : 0);
        lfn.attr     = ATTR_LFN;
        lfn.type     = 0;
        lfn.checksum = checksum;
        lfn.cluster  = 0;

        ascii_to_utf16le(seg + 0,  lfn.name1, 5);
        ascii_to_utf16le(seg + 5,  lfn.name2, 6);
        ascii_to_utf16le(seg + 11, lfn.name3, 2);

        /* Заполняем 0xFFFF после нулевого символа */
        for (int ci = 0; ci < 5; ci++)
            if (seg[ci] == 0 && ci > 0) lfn.name1[ci] = 0xFFFF;
        for (int ci = 0; ci < 6; ci++)
            if (5 + ci < 13 && seg[5 + ci] == 0) lfn.name2[ci] = 0xFFFF;

        WRITE_DIRENTRY(&lfn);
    }

    /* Основная 8.3 запись */
    fat_dir_entry_t de;
    kmem_set(&de, 0, sizeof(de));
    kmem_cpy(de.name, name83, 11);
    de.attr             = attr;
    de.first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de.first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    de.file_size        = file_size;
    /* Дата: 1 января 2024 = год 44, месяц 1, день 1 → (44<<9)|(1<<5)|1 */
    de.wrt_date = (uint16_t)(((2024 - 1980) << 9) | (1 << 5) | 1);
    de.crt_date = de.wrt_date;
    de.last_access_date = de.wrt_date;

    if (out_entry_lba) *out_entry_lba = cur_lba;
    if (out_entry_eis) *out_entry_eis = cur_eis;

    WRITE_DIRENTRY(&de);

    #undef WRITE_DIRENTRY

    return VFS_OK;
}

/* Обновить запись директории на диске (размер, кластер) */
static int fat_update_dir_entry(fat32_priv_t *fat,
                                 uint32_t lba, uint32_t eis,
                                 uint32_t first_cluster, uint64_t file_size) {
    if (fat->readonly) return VFS_ERR_ROFS;
    uint8_t tmp[512];
    if (blkdev_read(fat->dev, lba, 1, tmp) != VFS_OK) return VFS_ERR_IO;
    fat_dir_entry_t *e = (fat_dir_entry_t *)(tmp + eis * sizeof(fat_dir_entry_t));
    e->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    e->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    e->file_size        = (uint32_t)file_size;
    /* Обновляем дату изменения */
    e->wrt_date = (uint16_t)(((2024 - 1980) << 9) | (1 << 5) | 1);
    return blkdev_write(fat->dev, lba, 1, tmp) == VFS_OK ? VFS_OK : VFS_ERR_IO;
}

/* Пометить записи директории как удалённые (0xE5) */
static int fat_delete_dir_entries(fat32_priv_t *fat,
                                   uint32_t first_lba, uint32_t first_eis,
                                   uint32_t last_lba, uint32_t last_eis) {
    if (fat->readonly) return VFS_ERR_ROFS;
    uint32_t eps = fat->bytes_per_sector / sizeof(fat_dir_entry_t);
    uint8_t tmp[512];
    uint32_t cur_lba = first_lba;
    uint32_t cur_eis = first_eis;

    while (1) {
        if (blkdev_read(fat->dev, cur_lba, 1, tmp) != VFS_OK) return VFS_ERR_IO;
        while (cur_eis < eps) {
            fat_dir_entry_t *e = (fat_dir_entry_t *)(tmp + cur_eis * sizeof(fat_dir_entry_t));
            e->name[0] = FAT_ENTRY_FREE;
            if (cur_lba == last_lba && cur_eis == last_eis) {
                blkdev_write(fat->dev, cur_lba, 1, tmp);
                return VFS_OK;
            }
            cur_eis++;
        }
        if (blkdev_write(fat->dev, cur_lba, 1, tmp) != VFS_OK) return VFS_ERR_IO;
        cur_eis = 0;
        cur_lba++;
    }
}

/* =========================================================================
 * VFS node операции (vfs_node_ops_t реализация)
 * ========================================================================= */

/* Вспомогательная функция: создать vfs_node_t из fat_node_t */
static vfs_node_t *fat_make_vfs_node(fat32_priv_t *fat, fat_node_t *fn);

static int64_t fat_node_read(vfs_node_t *node, uint64_t offset,
                              uint64_t size, void *buf) {
    fat_node_t *fn = (fat_node_t *)node->fs_priv;
    fat32_priv_t *fat = fn->fat;

    if (fn->attr & ATTR_DIRECTORY) return VFS_ERR_ISDIR;
    if (offset >= fn->size) return 0;
    if (offset + size > fn->size) size = fn->size - offset;
    if (size == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    uint64_t total_read = 0;
    uint32_t bpc = fat->bytes_per_cluster;

    /* Номер кластера и байт внутри кластера */
    uint32_t cluster_idx  = (uint32_t)(offset / bpc);
    uint32_t byte_in_clus = (uint32_t)(offset % bpc);

    /* Следуем по цепочке до нужного кластера */
    uint32_t cur_cluster = fn->first_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        cur_cluster = fat_get_entry(fat, cur_cluster);
        if (cur_cluster >= FAT32_EOC || cur_cluster == FAT32_FREE)
            return (int64_t)total_read;
    }

    uint8_t *clus_buf = (uint8_t *)kmalloc(bpc);
    if (!clus_buf) return VFS_ERR_NOMEM;

    while (size > 0 && cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        if (fat_read_cluster(fat, cur_cluster, clus_buf) != VFS_OK) {
            kfree(clus_buf);
            return total_read > 0 ? (int64_t)total_read : VFS_ERR_IO;
        }

        uint64_t to_copy = bpc - byte_in_clus;
        if (to_copy > size) to_copy = size;

        kmem_cpy(out, clus_buf + byte_in_clus, to_copy);
        out        += to_copy;
        total_read += to_copy;
        size       -= to_copy;
        byte_in_clus = 0;

        if (size > 0) {
            cur_cluster = fat_get_entry(fat, cur_cluster);
        }
    }

    kfree(clus_buf);
    return (int64_t)total_read;
}

static int64_t fat_node_write(vfs_node_t *node, uint64_t offset,
                               uint64_t size, const void *buf) {
    fat_node_t *fn = (fat_node_t *)node->fs_priv;
    fat32_priv_t *fat = fn->fat;

    if (fat->readonly) return VFS_ERR_ROFS;
    if (fn->attr & ATTR_DIRECTORY) return VFS_ERR_ISDIR;
    if (fn->attr & ATTR_READ_ONLY) return VFS_ERR_ROFS;
    if (size == 0) return 0;

    uint32_t bpc = fat->bytes_per_cluster;
    const uint8_t *inp = (const uint8_t *)buf;
    uint64_t total_written = 0;

    /* Если файл пустой — выделяем первый кластер */
    if (fn->first_cluster < 2) {
        uint32_t c = fat_alloc_cluster(fat);
        if (c == 0) return VFS_ERR_NOSPC;
        fn->first_cluster = c;
        /* Обнуляем кластер */
        uint8_t *zero = (uint8_t *)kzalloc(bpc);
        if (zero) { fat_write_cluster(fat, c, zero); kfree(zero); }
    }

    uint32_t cluster_idx  = (uint32_t)(offset / bpc);
    uint32_t byte_in_clus = (uint32_t)(offset % bpc);

    /* Следуем/расширяем цепочку до нужного кластера */
    uint32_t cur_cluster = fn->first_cluster;
    uint32_t prev_cluster = 0;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        uint32_t next = fat_get_entry(fat, cur_cluster);
        if (next >= FAT32_EOC || next == FAT32_FREE) {
            /* Расширяем цепочку */
            uint32_t new_c = fat_extend_chain(fat, cur_cluster);
            if (new_c == 0) goto done;
            uint8_t *zero = (uint8_t *)kzalloc(bpc);
            if (zero) { fat_write_cluster(fat, new_c, zero); kfree(zero); }
            cur_cluster = new_c;
        } else {
            prev_cluster = cur_cluster;
            cur_cluster = next;
        }
        (void)prev_cluster;
    }

    uint8_t *clus_buf = (uint8_t *)kmalloc(bpc);
    if (!clus_buf) return VFS_ERR_NOMEM;

    while (size > 0) {
        /* Читаем текущий кластер */
        if (byte_in_clus != 0 || size < bpc) {
            if (fat_read_cluster(fat, cur_cluster, clus_buf) != VFS_OK) {
                kfree(clus_buf);
                goto done;
            }
        } else {
            kmem_set(clus_buf, 0, bpc);
        }

        uint64_t to_copy = bpc - byte_in_clus;
        if (to_copy > size) to_copy = size;

        kmem_cpy(clus_buf + byte_in_clus, inp, to_copy);

        if (fat_write_cluster(fat, cur_cluster, clus_buf) != VFS_OK) {
            kfree(clus_buf);
            goto done;
        }

        inp           += to_copy;
        total_written += to_copy;
        size          -= to_copy;
        byte_in_clus   = 0;

        if (size > 0) {
            uint32_t next = fat_get_entry(fat, cur_cluster);
            if (next >= FAT32_EOC || next == FAT32_FREE) {
                next = fat_extend_chain(fat, cur_cluster);
                if (next == 0) { kfree(clus_buf); goto done; }
                uint8_t *zero = (uint8_t *)kzalloc(bpc);
                if (zero) { fat_write_cluster(fat, next, zero); kfree(zero); }
            }
            cur_cluster = next;
        }
    }
    kfree(clus_buf);

done:
    /* Обновляем размер */
    if (offset + total_written > fn->size)
        fn->size = offset + total_written;
    node->stat.size = fn->size;
    fn->dirty = true;

    /* Обновляем запись директории */
    fat_update_dir_entry(fat, fn->dir_entry_idx >> 16,
                         fn->dir_entry_idx & 0xFFFF,
                         fn->first_cluster, fn->size);
    /* Примечание: dir_entry_idx хранит lba<<16|eis — см. lookup */

    fat_flush_fat(fat);
    fat_write_fsinfo(fat);

    return (int64_t)total_written;
}

static int fat_node_readdir(vfs_node_t *node, uint32_t index, vfs_dirent_t *out) {
    fat_node_t *fn = (fat_node_t *)node->fs_priv;
    if (!(fn->attr & ATTR_DIRECTORY)) return VFS_ERR_NOTDIR;
    return fat_dir_read_nth(fn->fat, fn->first_cluster, index, out);
}

static vfs_node_t *fat_node_lookup(vfs_node_t *parent, const char *name) {
    fat_node_t *pfn = (fat_node_t *)parent->fs_priv;
    if (!(pfn->attr & ATTR_DIRECTORY)) return NULL;

    fat_found_t found;
    if (fat_dir_find(pfn->fat, pfn->first_cluster, name, &found) != VFS_OK)
        return NULL;

    fat_node_t *fn = (fat_node_t *)kzalloc(sizeof(fat_node_t));
    if (!fn) return NULL;

    fn->fat           = pfn->fat;
    fn->first_cluster = ((uint32_t)found.entry.first_cluster_hi << 16)
                       | found.entry.first_cluster_lo;
    fn->size          = found.entry.file_size;
    fn->attr          = found.entry.attr;
    fn->dirty         = false;
    fn->dir_cluster   = pfn->first_cluster;
    /* Пакуем lba<<16|eis в dir_entry_idx (грубо, достаточно для большинства ФС) */
    fn->dir_entry_idx = (found.entry_lba << 16) | found.entry_in_sector;
    fn->is_root       = false;

    vfs_node_t *n = fat_make_vfs_node(pfn->fat, fn);
    if (!n) { kfree(fn); return NULL; }
    return n;
}

static vfs_node_t *fat_node_create(vfs_node_t *parent, const char *name, uint8_t type) {
    fat_node_t *pfn = (fat_node_t *)parent->fs_priv;
    if (!(pfn->attr & ATTR_DIRECTORY)) return NULL;
    fat32_priv_t *fat = pfn->fat;
    if (fat->readonly) return NULL;

    uint8_t attr = ATTR_ARCHIVE;
    if (type == VFS_TYPE_DIR) attr = ATTR_DIRECTORY;

    /* Для директорий выделяем первый кластер сразу */
    uint32_t first_cluster = 0;
    if (type == VFS_TYPE_DIR) {
        first_cluster = fat_alloc_cluster(fat);
        if (first_cluster == 0) return NULL;

        /* Заполняем кластер нулями */
        uint8_t *zero = (uint8_t *)kzalloc(fat->bytes_per_cluster);
        if (!zero) { fat_set_entry(fat, first_cluster, FAT32_FREE); return NULL; }
        fat_write_cluster(fat, first_cluster, zero);
        kfree(zero);

        /* Создаём "." и ".." */
        uint32_t dot_lba, dot_eis;
        /* Запись "." */
        fat_dir_add_entry(fat, first_cluster, ".", ATTR_DIRECTORY,
                          first_cluster, 0, &dot_lba, &dot_eis);
        /* Запись ".." */
        fat_dir_add_entry(fat, first_cluster, "..", ATTR_DIRECTORY,
                          pfn->first_cluster, 0, NULL, NULL);
    }

    uint32_t entry_lba, entry_eis;
    int r = fat_dir_add_entry(fat, pfn->first_cluster, name, attr,
                               first_cluster, 0, &entry_lba, &entry_eis);
    if (r != VFS_OK) {
        if (type == VFS_TYPE_DIR) fat_free_chain(fat, first_cluster);
        return NULL;
    }

    fat_flush_fat(fat);
    fat_write_fsinfo(fat);

    fat_node_t *fn = (fat_node_t *)kzalloc(sizeof(fat_node_t));
    if (!fn) return NULL;
    fn->fat           = fat;
    fn->first_cluster = first_cluster;
    fn->size          = 0;
    fn->attr          = attr;
    fn->dirty         = false;
    fn->dir_cluster   = pfn->first_cluster;
    fn->dir_entry_idx = (entry_lba << 16) | entry_eis;
    fn->is_root       = false;

    vfs_node_t *n = fat_make_vfs_node(fat, fn);
    if (!n) { kfree(fn); return NULL; }
    return n;
}

static int fat_node_unlink(vfs_node_t *parent, const char *name) {
    fat_node_t *pfn = (fat_node_t *)parent->fs_priv;
    fat32_priv_t *fat = pfn->fat;
    if (fat->readonly) return VFS_ERR_ROFS;

    fat_found_t found;
    if (fat_dir_find(pfn->fat, pfn->first_cluster, name, &found) != VFS_OK)
        return VFS_ERR_NOENT;

    /* Если директория — проверяем, что она пуста */
    if (found.entry.attr & ATTR_DIRECTORY) {
        uint32_t child_cluster = ((uint32_t)found.entry.first_cluster_hi << 16)
                               | found.entry.first_cluster_lo;
        vfs_dirent_t de;
        int r = fat_dir_read_nth(fat, child_cluster, 0, &de);
        if (r == VFS_OK) return VFS_ERR_BUSY; /* не пустая */
    }

    /* Освобождаем цепочку кластеров файла */
    uint32_t first_cluster = ((uint32_t)found.entry.first_cluster_hi << 16)
                           | found.entry.first_cluster_lo;
    if (first_cluster >= 2) fat_free_chain(fat, first_cluster);

    /* Помечаем LFN и основную запись как удалённые */
    fat_delete_dir_entries(fat,
        found.lfn_start_lba, found.lfn_start_eis,
        found.entry_lba,     found.entry_in_sector);

    fat_flush_fat(fat);
    fat_write_fsinfo(fat);
    return VFS_OK;
}

static int fat_node_truncate(vfs_node_t *node, uint64_t new_size) {
    fat_node_t *fn = (fat_node_t *)node->fs_priv;
    fat32_priv_t *fat = fn->fat;
    if (fat->readonly) return VFS_ERR_ROFS;

    uint32_t bpc = fat->bytes_per_cluster;
    uint32_t needed_clusters = (uint32_t)((new_size + bpc - 1) / bpc);

    if (new_size == 0) {
        /* Освобождаем всё */
        if (fn->first_cluster >= 2)
            fat_free_chain(fat, fn->first_cluster);
        fn->first_cluster = 0;
        fn->size = 0;
    } else if (new_size < fn->size) {
        /* Укорачиваем цепочку */
        uint32_t cur = fn->first_cluster;
        for (uint32_t i = 1; i < needed_clusters; i++) {
            uint32_t next = fat_get_entry(fat, cur);
            if (next >= FAT32_EOC) break;
            cur = next;
        }
        /* cur — последний нужный кластер */
        uint32_t tail = fat_get_entry(fat, cur);
        fat_set_entry(fat, cur, FAT32_EOC);
        if (tail >= 2 && tail < FAT32_EOC)
            fat_free_chain(fat, tail);
        fn->size = new_size;
    } else {
        /* Расширяем (заполняем нулями) */
        /* Находим последний кластер */
        uint32_t cur = fn->first_cluster;
        if (cur < 2) {
            cur = fat_alloc_cluster(fat);
            if (cur == 0) return VFS_ERR_NOSPC;
            fn->first_cluster = cur;
            uint8_t *z = (uint8_t *)kzalloc(bpc);
            if (z) { fat_write_cluster(fat, cur, z); kfree(z); }
        } else {
            uint32_t next;
            while ((next = fat_get_entry(fat, cur)) < FAT32_EOC)
                cur = next;
        }
        uint32_t current_clusters = fat_chain_length(fat, fn->first_cluster);
        for (uint32_t i = current_clusters; i < needed_clusters; i++) {
            uint32_t new_c = fat_extend_chain(fat, cur);
            if (new_c == 0) break;
            uint8_t *z = (uint8_t *)kzalloc(bpc);
            if (z) { fat_write_cluster(fat, new_c, z); kfree(z); }
            cur = new_c;
        }
        fn->size = new_size;
    }

    node->stat.size = fn->size;
    fn->dirty = true;

    fat_update_dir_entry(fat, fn->dir_entry_idx >> 16,
                         fn->dir_entry_idx & 0xFFFF,
                         fn->first_cluster, fn->size);
    fat_flush_fat(fat);
    fat_write_fsinfo(fat);
    return VFS_OK;
}

static int fat_node_sync(vfs_node_t *node) {
    fat_node_t *fn = (fat_node_t *)node->fs_priv;
    if (!fn->dirty) return VFS_OK;
    fat_update_dir_entry(fn->fat, fn->dir_entry_idx >> 16,
                         fn->dir_entry_idx & 0xFFFF,
                         fn->first_cluster, fn->size);
    fat_flush_fat(fn->fat);
    fat_write_fsinfo(fn->fat);
    fn->dirty = false;
    return VFS_OK;
}

static void fat_node_release(vfs_node_t *node) {
    if (!node) return;

    /* КРИТИЧНО: узел нельзя освобождать безусловно — у него может быть
     * больше одной активной ссылки. В частности, корень смонтированной ФС
     * (mnt->root) держит ПОСТОЯННУЮ ссылку весь срок жизни монтирования;
     * vfs_resolve() временно инкрементирует refcount при каждом проходе
     * через узел и вызывает release() по завершении прохода — это должно
     * лишь снять временную ссылку, а не разрушить узел, пока постоянная
     * ссылка ещё жива.
     *
     * Раньше здесь refcount полностью игнорировался, и узел освобождался
     * при первом же вызове release(). Из-за этого корень ФС становился
     * "мёртвым" указателем (mnt->root) уже после первого обращения к файлу,
     * а его память могла быть переиспользована под другие структуры —
     * это и приводило к чтению NULL/мусорных полей (например,
     * fat32_priv_t->total_clusters по смещению 0x1C) и падению ядра.
     */
    if (node->refcount > 1) {
        node->refcount--;
        return;
    }
    node->refcount = 0;

    fat_node_t *fn = (fat_node_t *)node->fs_priv;
    if (fn) {
        if (fn->dirty) fat_node_sync(node);
        kfree(fn);
    }
    kfree(node);
}

/* =========================================================================
 * Таблица операций и фабрика узлов
 * ========================================================================= */
static vfs_node_ops_t g_fat_node_ops = {
    .read    = fat_node_read,
    .write   = fat_node_write,
    .readdir = fat_node_readdir,
    .lookup  = fat_node_lookup,
    .create  = fat_node_create,
    .unlink  = fat_node_unlink,
    .sync    = fat_node_sync,
    .release = fat_node_release,
    .truncate = fat_node_truncate,
};

static vfs_node_t *fat_make_vfs_node(fat32_priv_t *fat, fat_node_t *fn) {
    vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    n->ops       = &g_fat_node_ops;
    n->fs_priv   = fn;
    n->refcount  = 1;
    n->stat.type = (fn->attr & ATTR_DIRECTORY) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    n->stat.size = fn->size;
    n->stat.alloc_size = (uint64_t)fat_chain_length(fat, fn->first_cluster)
                       * fat->bytes_per_cluster;
    n->stat.attr    = fn->attr;
    n->stat.cluster = fn->first_cluster;
    return n;
}

/* =========================================================================
 * Монтирование FAT (mount/umount)
 * ========================================================================= */
static vfs_node_t *fat_mount(blkdev_t *dev, uint32_t flags) {
    uint8_t *boot_sec = (uint8_t *)kzalloc(512);
    if (!boot_sec) return NULL;

    DBG_MSG("FAT", "fat_mount: reading boot sector...");
    DBG_VAL("FAT", "boot_sec ptr", (uint64_t)(uintptr_t)boot_sec);
    if (blkdev_read(dev, 0, 1, boot_sec) != VFS_OK) {
        kfree(boot_sec);
        DBG_MSG("FAT", "failed to read boot sector");
        DBG_MSG("FAT", ">>> virtio-blk timeout: check desc flags, dma phys addr, avail->flags=1");
        return NULL;
    }
    /* Дамп первых 16 байт загрузочного сектора */
    { const uint8_t *b = boot_sec;
      dbg_puts("[FAT] boot[0..15]: ");
      static const char hx[] = "0123456789ABCDEF";
      for (int _i = 0; _i < 16; _i++) { dbg_putc(hx[b[_i]>>4]); dbg_putc(hx[b[_i]&0xF]); dbg_putc(' '); }
      dbg_puts("\n");
    }
    DBG_VAL("FAT", "boot sig at 0x1FE (should be 0xAA55)", (uint64_t)((uint16_t)boot_sec[0x1FE] | ((uint16_t)boot_sec[0x1FF]<<8)));
    DBG_VAL("FAT", "jmp[0] (EB or E9?)", (uint64_t)boot_sec[0]);
    DBG_VAL("FAT", "bytes_per_sector raw", (uint64_t)((uint16_t)boot_sec[0x0B] | ((uint16_t)boot_sec[0x0C]<<8)));

    fat_bpb_t *bpb = (fat_bpb_t *)boot_sec;

    /* Проверяем сигнатуры */
    DBG_VAL("FAT", "bpb->boot_signature2", (uint64_t)bpb->boot_signature2);
    DBG_VAL("FAT", "bpb->bytes_per_sector", (uint64_t)bpb->bytes_per_sector);
    DBG_VAL("FAT", "bpb->sectors_per_cluster", (uint64_t)bpb->sectors_per_cluster);
    DBG_VAL("FAT", "bpb->reserved_sectors", (uint64_t)bpb->reserved_sectors);
    DBG_VAL("FAT", "bpb->num_fats", (uint64_t)bpb->num_fats);
    DBG_VAL("FAT", "bpb->fat_size_16", (uint64_t)bpb->fat_size_16);
    DBG_VAL("FAT", "bpb->fat_size_32", (uint64_t)bpb->fat_size_32);
    DBG_VAL("FAT", "bpb->total_sectors_16", (uint64_t)bpb->total_sectors_16);
    DBG_VAL("FAT", "bpb->total_sectors_32", (uint64_t)bpb->total_sectors_32);
    DBG_VAL("FAT", "bpb->root_cluster", (uint64_t)bpb->root_cluster);
    { dbg_puts("[FAT] fs_type: ");
      for (int _i = 0; _i < 8; _i++) dbg_putc(bpb->fs_type[_i] >= 32 ? bpb->fs_type[_i] : '?');
      dbg_puts("\n");
    }
    if (bpb->boot_signature2 != 0xAA55) {
        DBG_MSG("FAT", "bad boot signature");
        kfree(boot_sec);
        return NULL;
    }
    if (bpb->bytes_per_sector != 512 && bpb->bytes_per_sector != 1024 &&
        bpb->bytes_per_sector != 2048 && bpb->bytes_per_sector != 4096) {
        DBG_MSG("FAT", "bad bytes_per_sector");
        kfree(boot_sec);
        return NULL;
    }

    fat32_priv_t *fat = (fat32_priv_t *)kzalloc(sizeof(fat32_priv_t));
    if (!fat) { kfree(boot_sec); return NULL; }

    fat->dev                 = dev;
    fat->bytes_per_sector    = bpb->bytes_per_sector;
    fat->sectors_per_cluster = bpb->sectors_per_cluster;
    fat->sectors_per_cluster32 = bpb->sectors_per_cluster;
    fat->reserved_sectors    = bpb->reserved_sectors;
    fat->num_fats            = bpb->num_fats;
    fat->fs_info_sector      = 0;
    fat->readonly            = !!(flags & MS_RDONLY) || dev->readonly;

    /* Определяем тип FAT и параметры */
    uint32_t fat_size = bpb->fat_size_16 ? bpb->fat_size_16 : bpb->fat_size_32;
    fat->fat_size = fat_size;

    uint32_t total_sectors = bpb->total_sectors_16 ?
                             bpb->total_sectors_16 : bpb->total_sectors_32;

    uint32_t root_dir_sectors = ((bpb->root_entry_count * 32)
                                + bpb->bytes_per_sector - 1)
                               / bpb->bytes_per_sector;

    fat->first_fat_sector  = bpb->reserved_sectors;
    fat->data_start_sector = bpb->reserved_sectors
                           + bpb->num_fats * fat_size
                           + root_dir_sectors;

    uint32_t data_sectors = total_sectors - fat->data_start_sector;
    fat->total_clusters   = data_sectors / bpb->sectors_per_cluster;

    /* FAT type determination (по спецификации Microsoft) */
    if (fat->total_clusters < 4085) {
        fat->fat_type = FAT_TYPE_12;
        DBG_MSG("FAT", "type: FAT12");
    } else if (fat->total_clusters < 65525) {
        fat->fat_type = FAT_TYPE_16;
        DBG_MSG("FAT", "type: FAT16");
    } else {
        fat->fat_type = FAT_TYPE_32;
        DBG_MSG("FAT", "type: FAT32");
        fat->root_cluster   = bpb->root_cluster;
        fat->fs_info_sector = bpb->fs_info_sector;
    }

    fat->bytes_per_cluster = (uint32_t)bpb->bytes_per_sector
                           * bpb->sectors_per_cluster;
    DBG_VAL("FAT", "computed: total_clusters",   (uint64_t)fat->total_clusters);
    DBG_VAL("FAT", "computed: data_start_sector",(uint64_t)fat->data_start_sector);
    DBG_VAL("FAT", "computed: first_fat_sector", (uint64_t)fat->first_fat_sector);
    DBG_VAL("FAT", "computed: fat_size",         (uint64_t)fat->fat_size);
    DBG_VAL("FAT", "computed: bytes_per_cluster",(uint64_t)fat->bytes_per_cluster);

    /* FAT12/16: корень перед областью данных */
    if (fat->fat_type != FAT_TYPE_32)
        fat->root_cluster = 0; /* особый код */
    else
        fat->root_cluster = bpb->root_cluster;

    kfree(boot_sec);

    /* Читаем FSInfo */
    fat->free_cluster_count = 0xFFFFFFFF;
    fat->next_free_cluster  = 2;
    if (fat->fat_type == FAT_TYPE_32 && fat->fs_info_sector != 0) {
        uint8_t *fi_buf = (uint8_t *)kzalloc(512);
        if (fi_buf && blkdev_read(dev, fat->fs_info_sector, 1, fi_buf) == VFS_OK) {
            fat_fsinfo_t *fi = (fat_fsinfo_t *)fi_buf;
            if (fi->lead_sig   == FSINFO_LEAD_SIG   &&
                fi->struct_sig == FSINFO_STRUCT_SIG  &&
                fi->trail_sig  == FSINFO_TRAIL_SIG) {
                fat->free_cluster_count = fi->free_count;
                fat->next_free_cluster  = fi->next_free;
                if (fat->next_free_cluster < 2)
                    fat->next_free_cluster = 2;
            }
        }
        if (fi_buf) kfree(fi_buf);
    }

    /* Кэшируем FAT если она влезает */
    uint64_t fat_bytes = (uint64_t)fat_size * fat->bytes_per_sector;
    if (fat_bytes <= FAT_CACHE_MAX) {
        fat->fat_cache = (uint8_t *)kmalloc(fat_bytes);
        if (fat->fat_cache) {
            if (blkdev_read(dev, fat->first_fat_sector,
                            fat_size, fat->fat_cache) == VFS_OK) {
                fat->fat_cache_sectors = fat_size;
                fat->fat_cache_dirty   = false;
                DBG_VAL("FAT", "FAT cached sectors", fat_size);
            } else {
                kfree(fat->fat_cache);
                fat->fat_cache = NULL;
            }
        }
    } else {
        fat->fat_cache = NULL;
        DBG_MSG("FAT", "FAT too large to cache");
    }

    /* Сектор-буфер */
    fat->sector_buf = (uint8_t *)kmalloc(fat->bytes_per_sector);
    if (!fat->sector_buf) {
        if (fat->fat_cache) kfree(fat->fat_cache);
        kfree(fat);
        return NULL;
    }
    fat->sector_buf_lba   = (uint32_t)-1;
    fat->sector_buf_dirty = false;

    DBG_VAL("FAT", "bytes_per_sector",    fat->bytes_per_sector);
    DBG_VAL("FAT", "sectors_per_cluster", fat->sectors_per_cluster);
    DBG_VAL("FAT", "total_clusters",      fat->total_clusters);
    DBG_VAL("FAT", "root_cluster",        fat->root_cluster);
    DBG_VAL("FAT", "data_start",          fat->data_start_sector);
    DBG_VAL("FAT", "free_clusters",       fat->free_cluster_count);

    /* Создаём корневой vfs_node */
    fat_node_t *root_fn = (fat_node_t *)kzalloc(sizeof(fat_node_t));
    if (!root_fn) {
        kfree(fat->sector_buf);
        if (fat->fat_cache) kfree(fat->fat_cache);
        kfree(fat);
        return NULL;
    }
    root_fn->fat           = fat;
    root_fn->first_cluster = fat->root_cluster;
    root_fn->size          = 0;
    root_fn->attr          = ATTR_DIRECTORY;
    root_fn->is_root       = true;
    root_fn->dirty         = false;

    vfs_node_t *root = fat_make_vfs_node(fat, root_fn);
    if (!root) {
        kfree(root_fn);
        kfree(fat->sector_buf);
        if (fat->fat_cache) kfree(fat->fat_cache);
        kfree(fat);
        return NULL;
    }

    DBG_MSG("FAT", "mount ok");
    return root;
}

static void fat_umount(vfs_mount_t *mnt) {
    if (!mnt || !mnt->root) return;
    fat_node_t *root_fn = (fat_node_t *)mnt->root->fs_priv;
    if (!root_fn) return;
    fat32_priv_t *fat = root_fn->fat;
    if (!fat) return;

    /* Сбрасываем кэши */
    fat_flush_fat(fat);
    fat_write_fsinfo(fat);

    if (fat->sector_buf_dirty && fat->sector_buf_lba != (uint32_t)-1)
        blkdev_write(fat->dev, fat->sector_buf_lba, 1, fat->sector_buf);

    if (fat->fat_cache) kfree(fat->fat_cache);
    if (fat->sector_buf) kfree(fat->sector_buf);
    kfree(fat);
}

/* =========================================================================
 * Регистрация драйвера FAT32
 * ========================================================================= */
static fs_ops_t g_fat32_ops = {
    .mount  = fat_mount,
    .umount = fat_umount,
    .name   = "fat32",
};

int fat32_register(void) {
    return vfs_register_fs(&g_fat32_ops);
}
