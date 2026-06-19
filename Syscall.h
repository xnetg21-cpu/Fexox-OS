/*
 * Syscall.h — системные вызовы FEXOS
 *
 * Таблица номеров (совпадает с Linux x86_64 где это удобно):
 *   0  — sys_read
 *   1  — sys_write
 *   2  — sys_open
 *   3  — sys_close
 *   4  — sys_stat
 *   5  — sys_fstat
 *   8  — sys_lseek
 *   9  — sys_mmap
 *   11 — sys_munmap
 *   10 — sys_mprotect
 *   12 — sys_brk
 *   20 — sys_getpid
 *   24 — sys_sched_yield
 *   39 — sys_getpid  (alias, Linux compat)
 *   56 — sys_clone   (упрощённый, потоки)
 *   57 — sys_fork    (COW-fork, упрощённый)
 *   59 — sys_execve
 *   60 — sys_exit
 *   61 — sys_wait4
 *   72 — sys_fcntl
 *   80 — sys_chdir
 *   82 — sys_rename
 *   83 — sys_mkdir
 *   84 — sys_rmdir
 *   87 — sys_unlink
 *   89 — sys_readdir
 *   91 — sys_ftruncate
 *   158 — sys_arch_prctl  (минимальный: SET_FS только)
 *   500 — sys_get_framebuffer  (кастомный FEXOS)
 *   502 — sys_pipe
 *   503 — sys_shm_get
 *   504 — sys_shm_at
 *   505 — sys_shm_detach
 *
 * Соглашение о вызовах (System V AMD64):
 *   syscall#  : rax
 *   аргументы : rdi, rsi, rdx, r10, r8, r9
 *   возврат   : rax (< 0 — errno отрицательный)
 *
 * Сборка syscall_entry (naked, в main.c):
 *   swapgs → сохранить user rsp → переключиться на g_syscall_kstack →
 *   сохранить rcx (user rip) и r11 (user rflags) → call syscall_dispatch →
 *   восстановить → swapgs → sysretq
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Номера системных вызовов
 * ========================================================================= */
#define SYS_READ          0
#define SYS_WRITE         1
#define SYS_OPEN          2
#define SYS_CLOSE         3
#define SYS_STAT          4
#define SYS_FSTAT         5
#define SYS_LSEEK         8
#define SYS_MMAP          9
#define SYS_MPROTECT      10
#define SYS_MUNMAP        11
#define SYS_BRK           12
#define SYS_GETPID        20
#define SYS_SCHED_YIELD   24
#define SYS_GETPID2       39   /* Linux compat alias */
#define SYS_CLONE         56
#define SYS_FORK          57
#define SYS_EXECVE        59
#define SYS_EXIT          60
#define SYS_WAIT4         61
#define SYS_FCNTL         72
#define SYS_CHDIR         80
#define SYS_RENAME        82
#define SYS_MKDIR         83
#define SYS_RMDIR         84
#define SYS_UNLINK        87
#define SYS_READDIR       89
#define SYS_FTRUNCATE     91
#define SYS_ARCH_PRCTL    158
#define SYS_GET_FRAMEBUFFER 500  /* кастомный: адрес VGA/GOP буфера */
#define SYS_PIPE          502
#define SYS_SHM_GET       503
#define SYS_SHM_AT        504
#define SYS_SHM_DETACH    505

/* =========================================================================
 * Коды ошибок (отрицательные, как в POSIX)
 * ========================================================================= */
#define EPERM     1   /* операция не разрешена */
#define ENOENT    2   /* нет такого файла */
#define ESRCH     3   /* нет такого процесса */
#define EINTR     4   /* прерван сигналом */
#define EIO       5   /* ошибка ввода-вывода */
#define EBADF     9   /* неверный файловый дескриптор */
#define ECHILD    10  /* нет дочерних процессов */
#define ENOMEM    12  /* нет памяти */
#define EACCES    13  /* доступ запрещён */
#define EFAULT    14  /* неверный адрес в пространстве пользователя */
#define EEXIST    17  /* файл существует */
#define EXDEV     18  /* другое устройство */
#define ENOTDIR   20  /* не директория */
#define EISDIR    21  /* является директорией */
#define EINVAL    22  /* неверный аргумент */
#define ENFILE    23  /* таблица файлов переполнена */
#define EMFILE    24  /* слишком много открытых файлов */
#define ENOSPC    28  /* нет места на устройстве */
#define EROFS     30  /* файловая система только для чтения */
#define EPIPE     32  /* битый канал */
#define ERANGE    34  /* результат вне диапазона */
#define ENOSYS    38  /* функция не реализована */
#define ENOTEMPTY 39  /* директория не пустая */
#define EOVERFLOW 75  /* значение слишком большое */

/* =========================================================================
 * Флаги mmap / mprotect
 * ========================================================================= */
#define PROT_NONE   0x00
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANON      0x20
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void *)(uintptr_t)-1)

/* =========================================================================
 * Флаги clone (упрощённые)
 * ========================================================================= */
#define CLONE_VM      0x00000100  /* разделять память */
#define CLONE_FS      0x00000200  /* разделять файловую систему */
#define CLONE_FILES   0x00000400  /* разделять таблицу файловых дескрипторов */
#define CLONE_THREAD  0x00010000  /* поток (не процесс) */

/* =========================================================================
 * Флаги fcntl
 * ========================================================================= */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define FD_CLOEXEC 1

/* =========================================================================
 * Флаги arch_prctl
 * ========================================================================= */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004

/* =========================================================================
 * sys_stat / sys_fstat — структура результата (упрощённая)
 * ========================================================================= */
typedef struct {
    uint8_t  type;       /* VFS_TYPE_FILE / VFS_TYPE_DIR */
    uint64_t size;
    uint64_t alloc_size;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t atime;
    uint32_t attr;
    uint32_t cluster;
} sc_stat_t;

/* =========================================================================
 * Дескриптор разделяемой памяти (shm)
 * ========================================================================= */
#define SHM_MAX_REGIONS  32
#define SHM_MAX_NAME     64

typedef struct {
    char     name[SHM_MAX_NAME];
    uint64_t phys_base;    /* физический адрес региона */
    uint64_t virt_kernel;  /* виртуальный адрес в kernel space */
    uint64_t size;         /* размер в байтах */
    uint32_t pages;        /* количество страниц */
    uint32_t refcount;     /* счётчик подключений */
    bool     valid;
} shm_region_t;

/* =========================================================================
 * Дескриптор pipe
 * ========================================================================= */
#define PIPE_BUF_SIZE  4096
#define PIPE_MAX       64

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;       /* байт в буфере */
    bool     valid;
    bool     write_closed;
    bool     read_closed;
} pipe_t;

/* =========================================================================
 * Информация о framebuffer (sys_get_framebuffer)
 * Для VGA текстового режима — заглушка.
 * Когда появится GOP/framebuffer — заменить поля.
 * ========================================================================= */
typedef struct {
    uint64_t phys_addr;   /* физический адрес буфера */
    uint64_t virt_addr;   /* виртуальный адрес (mapped в user space) */
    uint32_t width;       /* ширина в пикселях (или столбцах для VGA) */
    uint32_t height;      /* высота */
    uint32_t pitch;       /* байт на строку */
    uint32_t bpp;         /* бит на пиксель (4 = VGA text, 32 = linear) */
    uint8_t  mode;        /* 0 = VGA text 80x25, 1 = linear framebuffer */
} sc_framebuffer_t;

/* =========================================================================
 * Публичный API (вызывается из syscall_dispatch в main.c)
 * ========================================================================= */

/*
 * syscall_init — инициализация подсистемы:
 *   - очищает таблицу fd процессов
 *   - инициализирует shm/pipe таблицы
 *   - Вызывать после usermode_init() в kernel_entry.
 */
void syscall_init(void);

/*
 * syscall_dispatch — диспетчер.
 * Вызывается из syscall_entry (naked asm в main.c) через call.
 * Возвращает результат в rax.
 */
int64_t syscall_dispatch(uint64_t nr,
                         uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5);

/*
 * syscall_validate_ptr — проверяет что указатель из userspace
 * указывает на доступную user-memory (va < USER_VIRT_LIMIT).
 * Возвращает true если безопасно разыменовывать.
 */
bool syscall_validate_ptr(const void *ptr, size_t size);

#endif /* SYSCALL_H */
