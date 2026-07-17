/*
 * VFS.h — Virtual File System + Block Layer public API
 * FEXOS — freestanding x86_64, no libc
 *
 * Архитектура:
 *   [syscall / kernel code]
 *          |
 *        [VFS]           ← единая точка входа: open/read/write/close/stat/readdir
 *          |
 *   [filesystem driver]  ← FAT32 (расширяемо до ext2, ISO9660, …)
 *          |
 *   [block device layer] ← virtio-blk (QEMU), расширяемо до AHCI, NVMe
 *          |
 *   [hardware / DMA]
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Коды ошибок
 * ========================================================================= */
#define VFS_OK          0
#define VFS_ERR_NOMEM   (-1)
#define VFS_ERR_NOENT   (-2)   /* файл/директория не найдены */
#define VFS_ERR_NOTDIR  (-3)   /* не директория */
#define VFS_ERR_ISDIR   (-4)   /* ожидался файл, получили директорию */
#define VFS_ERR_IO      (-5)   /* ошибка ввода-вывода */
#define VFS_ERR_INVAL   (-6)   /* неверный аргумент */
#define VFS_ERR_NOSPC   (-7)   /* нет места */
#define VFS_ERR_EXIST   (-8)   /* уже существует */
#define VFS_ERR_NXDEV   (-9)   /* нет такого устройства */
#define VFS_ERR_BUSY    (-10)  /* занято */
#define VFS_ERR_ROFS    (-11)  /* файловая система только для чтения */
#define VFS_ERR_NFILE   (-12)  /* слишком много открытых файлов */
#define VFS_ERR_BADF    (-13)  /* плохой файловый дескриптор */
#define VFS_ERR_OVERFLOW (-14) /* смещение вышло за пределы */
#define VFS_ERR_CORRUPT (-15)  /* повреждённая ФС */
#define VFS_ERR_UNSUP   (-16)  /* операция не поддерживается */

/* =========================================================================
 * Флаги открытия файла (vfs_open)
 * ========================================================================= */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x0800

/* =========================================================================
 * Типы файлов (vfs_stat.type)
 * ========================================================================= */
#define VFS_TYPE_FILE  1
#define VFS_TYPE_DIR   2
#define VFS_TYPE_LINK  3   /* будущее */

/* =========================================================================
 * Максимальные константы
 * ========================================================================= */
#define VFS_MAX_NAME      255
#define VFS_MAX_PATH      4096
#define VFS_MAX_FD        256    /* глобальная таблица файловых дескрипторов */
#define VFS_MAX_MOUNTS    16
#define VFS_MAX_BLKDEVS   8

/* =========================================================================
 * vfs_stat — метаданные файла/директории
 * ========================================================================= */
typedef struct {
    uint8_t  type;          /* VFS_TYPE_* */
    uint64_t size;          /* размер в байтах */
    uint64_t alloc_size;    /* выделено на диске (кратно кластеру) */
    uint32_t ctime;         /* время создания (FAT: секунды с 1980-01-01) */
    uint32_t mtime;         /* время модификации */
    uint32_t atime;         /* время доступа */
    uint32_t attr;          /* атрибуты (FAT: ATTR_*) */
    uint32_t cluster;       /* начальный кластер (FAT) / inode (ext2) */
} vfs_stat_t;

/* =========================================================================
 * vfs_dirent — одна запись директории при readdir
 * ========================================================================= */
typedef struct {
    char     name[VFS_MAX_NAME + 1];
    uint8_t  type;          /* VFS_TYPE_* */
    uint64_t size;
    uint32_t cluster;
} vfs_dirent_t;

/* =========================================================================
 * Форварды
 * ========================================================================= */
struct vfs_node;
struct vfs_mount;
struct blkdev;

/* =========================================================================
 * БЛОЧНЫЙ СЛОЙ
 * ========================================================================= */

/* Операции блочного устройства */
typedef struct blkdev_ops {
    /* Читает count секторов, начиная с lba, в buf */
    int (*read_sectors)(struct blkdev *dev, uint64_t lba,
                        uint32_t count, void *buf);
    /* Записывает count секторов из buf начиная с lba */
    int (*write_sectors)(struct blkdev *dev, uint64_t lba,
                         uint32_t count, const void *buf);
    /* Освобождает ресурсы устройства */
    void (*destroy)(struct blkdev *dev);
} blkdev_ops_t;

/* Дескриптор блочного устройства */
typedef struct blkdev {
    char           name[32];     /* "vda", "sda", … */
    uint32_t       sector_size;  /* обычно 512 или 4096 */
    uint64_t       sector_count; /* всего секторов */
    void          *priv;         /* приватные данные драйвера */
    blkdev_ops_t  *ops;
    bool           readonly;
} blkdev_t;

/* Зарегистрировать блочное устройство. Возвращает индекс или < 0 */
int  blkdev_register(blkdev_t *dev);

/* Найти устройство по имени */
blkdev_t *blkdev_find(const char *name);

/* Вспомогательные обёртки */
int blkdev_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf);
int blkdev_write(blkdev_t *dev, uint64_t lba, uint32_t count, const void *buf);

/* =========================================================================
 * ФАЙЛОВАЯ СИСТЕМА — интерфейс драйвера
 * ========================================================================= */

/* Операции над файловым узлом */
typedef struct vfs_node_ops {
    /* Читает size байт из файла (смещение offset) в buf.
       Возвращает кол-во прочитанных байт или < 0 при ошибке. */
    int64_t (*read)(struct vfs_node *node, uint64_t offset,
                    uint64_t size, void *buf);

    /* Записывает size байт из buf в файл начиная с offset.
       Возвращает кол-во записанных байт или < 0. */
    int64_t (*write)(struct vfs_node *node, uint64_t offset,
                     uint64_t size, const void *buf);

    /* Читает следующую запись директории.
       index — порядковый номер (0, 1, 2, …).
       Возвращает VFS_OK, VFS_ERR_NOENT (конец), или ошибку. */
    int (*readdir)(struct vfs_node *node, uint32_t index,
                   vfs_dirent_t *out);

    /* Ищет дочерний узел по имени. Возвращает новый vfs_node* или NULL. */
    struct vfs_node *(*lookup)(struct vfs_node *parent, const char *name);

    /* Создаёт файл или директорию. Возвращает новый vfs_node* или NULL. */
    struct vfs_node *(*create)(struct vfs_node *parent,
                               const char *name, uint8_t type);

    /* Удаляет файл или пустую директорию. */
    int (*unlink)(struct vfs_node *parent, const char *name);

    /* Обновляет метаданные (размер и т.п.) на диске. */
    int (*sync)(struct vfs_node *node);

    /* Освобождает память узла (не удаляет файл, просто free). */
    void (*release)(struct vfs_node *node);

    /* Усекает файл до новой длины. */
    int (*truncate)(struct vfs_node *node, uint64_t new_size);
} vfs_node_ops_t;

/* Файловый узел (in-memory inode) */
typedef struct vfs_node {
    vfs_stat_t      stat;
    vfs_node_ops_t *ops;
    struct vfs_mount *mount;  /* к какому маунту принадлежит */
    uint32_t        refcount;
    void           *fs_priv;  /* приватные данные драйвера ФС */
} vfs_node_t;

/* Операции файловой системы (монтирование/размонтирование) */
typedef struct fs_ops {
    /* Смонтировать ФС. Возвращает корневой узел или NULL. */
    vfs_node_t *(*mount)(blkdev_t *dev, uint32_t flags);
    /* Размонтировать: сбросить кэши, освободить ресурсы. */
    void (*umount)(struct vfs_mount *mnt);
    /* Имя типа ФС: "fat32", "ext2", … */
    const char *name;
} fs_ops_t;

/* Точка монтирования */
typedef struct vfs_mount {
    char        path[VFS_MAX_PATH];  /* "/", "/boot", … */
    vfs_node_t *root;
    blkdev_t   *dev;
    fs_ops_t   *fs;
    uint32_t    flags;               /* MS_RDONLY и т.п. */
    void       *fs_priv;             /* приватные данные ФС */
} vfs_mount_t;

/* =========================================================================
 * VFS API — использование ядром / syscall-слоем
 * ========================================================================= */

/* Инициализация VFS (вызвать один раз при старте) */
int  vfs_init(void);

/* Регистрация драйвера ФС */
int  vfs_register_fs(fs_ops_t *ops);

/* Монтирование устройства.
   dev_name — имя блочного устройства ("vda")
   fs_name  — тип ФС ("fat32")
   path     — точка монтирования ("/", "/home", …)
   flags    — MS_RDONLY и т.п. */
#define MS_RDONLY  1
int  vfs_mount(const char *dev_name, const char *fs_name,
               const char *path, uint32_t flags);

/* Размонтирование */
int  vfs_umount(const char *path);

/* Перечисление точек монтирования — для UI ("Мой компьютер" и т.п.),
 * не часть POSIX-подобного API выше.
 *   vfs_mount_count — сколько дисков сейчас смонтировано.
 *   vfs_mount_info  — путь монтирования и имя блочного устройства
 *                     ("vda" и т.п.) для диска idx (0..count-1).
 *                     Возвращает false если idx вне диапазона. */
int  vfs_mount_count(void);
bool vfs_mount_info(int idx, char *path_out, size_t path_sz,
                    char *dev_out, size_t dev_sz);

/* --- Работа с файлами --- */

/* Открыть файл/директорию. Возвращает fd >= 0 или < 0 при ошибке. */
int     vfs_open(const char *path, int flags);

/* Закрыть файловый дескриптор */
int     vfs_close(int fd);

/* Чтение: возвращает кол-во прочитанных байт или < 0 */
int64_t vfs_read(int fd, void *buf, uint64_t size);

/* Запись: возвращает кол-во записанных байт или < 0 */
int64_t vfs_write(int fd, const void *buf, uint64_t size);

/* Позиционирование. whence: 0=SET, 1=CUR, 2=END */
int64_t vfs_seek(int fd, int64_t offset, int whence);

/* Метаданные по пути */
int     vfs_stat(const char *path, vfs_stat_t *out);

/* Метаданные по fd */
int     vfs_fstat(int fd, vfs_stat_t *out);

/* Читать запись директории по индексу.
   Возвращает VFS_OK, VFS_ERR_NOENT (конец), или ошибку. */
int     vfs_readdir(int fd, uint32_t index, vfs_dirent_t *out);

/* Создать директорию */
int     vfs_mkdir(const char *path);

/* Удалить файл */
int     vfs_unlink(const char *path);

/* Удалить директорию (должна быть пустой) */
int     vfs_rmdir(const char *path);

/* Усечь файл */
int     vfs_truncate(const char *path, uint64_t size);
int     vfs_ftruncate(int fd, uint64_t size);

/* =========================================================================
 * Драйвер FAT32 — регистрация
 * ========================================================================= */
int fat32_register(void);

/* =========================================================================
 * Virtio-blk драйвер — регистрация
 * ========================================================================= */
int virtio_blk_init(void);

#endif /* VFS_H */
