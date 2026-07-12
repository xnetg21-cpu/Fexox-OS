/*
 * Syscall.c — реализация системных вызовов FEXOS
 *
 * Реализованы:
 *   Процессы   : exit, fork (упрощённый), clone (потоки), execve, wait4,
 *                getpid, sched_yield
 *   Память     : brk, mmap (anon+file), munmap, mprotect
 *   ФС / I/O   : read, write, open, close, stat, fstat, lseek,
 *                mkdir, rmdir, unlink, rename, chdir, readdir,
 *                ftruncate, fcntl
 *   Прочее     : arch_prctl (SET_FS/GET_FS), pipe,
 *                shm_get, shm_at, shm_detach
 *   Графика    : get_framebuffer (реальный GOP linear framebuffer, если
 *                экран уже в linear-режиме; VGA text 0xB8000 fallback
 *                иначе), win_create/win_close/win_move (тонкая обёртка
 *                над оконным менеджером Framebuffer.c — то же самое
 *                окно+кнопка на панели задач, что и у встроенных в
 *                ядро приложений, например "My Computer")
 *
 * Зависимости:
 *   MemoryControl : kmalloc/kfree, pmm_alloc_frames/pmm_free_frames,
 *                   vmm_map_user, vmm_map, vmm_unmap
 *   Scheduler     : sched_current, sched_find, sched_create_kthread,
 *                   sched_exit, sched_yield, sched_task_count
 *   VFS           : vfs_open/close/read/write/seek/stat/fstat/readdir/
 *                   mkdir/rmdir/unlink/truncate/ftruncate
 *   ELF64         : elf64_spawn
 *   klibc         : memset, memcpy, strncpy, strlen, strcmp
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "Syscall.h"
#include "VFS.h"
#include "Scheduler.h"
#include "ELF64.h"
#include "debug_out.h"

/* =========================================================================
 * Внешние зависимости
 * ========================================================================= */
extern void    *kmalloc(size_t size);
extern void     kfree(void *ptr);

/* --- Framebuffer.c: реальный GOP framebuffer + оконный менеджер ---
 * (нет общего Framebuffer.h-инклюда в остальных модулях ядра — см.
 * аналогичный паттерн внешних extern'ов в PS2Mouse.c/ui_extra.c/fxapp.c). */
extern int      fb_get_mode(void);
extern uint32_t fb_width(void);
extern uint32_t fb_height(void);
extern uint64_t fb_phys_addr(void);
extern uint32_t fb_pitch(void);
extern uint8_t  fb_bpp(void);
#define FB_MODE_LINEAR_ 2   /* совпадает с FB_MODE_LINEAR из Framebuffer.h */

extern int  ui_win_create(const char *title, uint32_t w, uint32_t h, int32_t owner_pid);
extern int  ui_win_close(int win_id);
extern int  ui_win_move(int win_id, int32_t x, int32_t y);
extern void ui_win_close_all_owned_by(int32_t owner_pid);
extern uint64_t pmm_alloc_frames(size_t count, size_t align);
extern void     pmm_free_frames(uint64_t pa, size_t count);
extern int      vmm_map_user(uint64_t va, uint64_t pa, bool rw, bool exec);
extern int      vmm_map(uint64_t va, uint64_t pa, uint64_t flags);
extern int      vmm_unmap(uint64_t va);

/* klibc */
extern void  *memset(void *dst, int c, size_t n);
extern void  *memcpy(void *dst, const void *src, size_t n);
extern char  *strncpy(char *dst, const char *src, size_t n);
extern size_t strlen(const char *s);
extern int    strcmp(const char *a, const char *b);
extern int    strncmp(const char *a, const char *b, size_t n);

/* =========================================================================
 * Константы
 * ========================================================================= */
#define PAGE_SIZE           4096ULL
#define PAGE_SHIFT          12
#define PAGE_MASK           (~(PAGE_SIZE - 1ULL))
#define ROUND_UP(n, a)      (((uint64_t)(n) + (uint64_t)(a) - 1ULL) & ~((uint64_t)(a) - 1ULL))
#define ROUND_DOWN(n, a)    ((uint64_t)(n) & ~((uint64_t)(a) - 1ULL))

/* Граница user virtual address space */
#define USER_VIRT_LIMIT     0x00007FFFFFFFFFFFULL

/* =========================================================================
 * Оконные системные вызовы — номера.
 *
 * ВАЖНО: определены здесь через #ifndef, чтобы не конфликтовать, если
 * они уже объявлены в Syscall.h. Правильное место для них — Syscall.h
 * (вместе с остальными SYS_*), рядом с SYS_GET_FRAMEBUFFER; секцию ниже
 * стоит перенести туда же при следующей правке заголовка, а также
 * продублировать в userspace libc-заголовке syscall-номеров, которым
 * пользуются .fxapp/ELF-приложения.
 * ========================================================================= */
#ifndef SYS_WIN_CREATE
#define SYS_WIN_CREATE   600
#endif
#ifndef SYS_WIN_CLOSE
#define SYS_WIN_CLOSE    601
#endif
#ifndef SYS_WIN_MOVE
#define SYS_WIN_MOVE     602
#endif

/* Heap растёт вверх от этой базы (per-process в реальном ядре — здесь глобально) */
#define USER_HEAP_BASE      0x0000000010000000ULL
#define USER_HEAP_MAX       0x0000000080000000ULL

/* mmap область — размещаем анонимные маппинги здесь */
#define USER_MMAP_BASE      0x0000000100000000ULL
#define USER_MMAP_LIMIT     0x0000300000000000ULL

/* VGA text mode физический адрес */
#define VGA_TEXT_PHYS       0x000B8000ULL
#define VGA_TEXT_COLS       80
#define VGA_TEXT_ROWS       25

/* VMM флаги (из MemoryControl.c) */
#define PTE_P   (1ULL << 0)   /* Present */
#define PTE_W   (1ULL << 1)   /* Write */
#define PTE_U   (1ULL << 3)   /* User */
#define PTE_NX  (1ULL << 63)  /* No-Execute */

/* DIRECT_MAP_OFFSET из MemoryControl.c */
#define DIRECT_MAP_OFFSET   0xFFFF880000000000ULL

/* =========================================================================
 * Таблица файловых дескрипторов
 *
 * В настоящей ОС она per-process. Пока у нас одно адресное пространство
 * и нет полного fork — делаем одну глобальную таблицу, достаточно для
 * демонстрации системных вызовов.
 * ========================================================================= */
#define SC_FD_MAX   256

typedef struct {
    bool    open;
    int     vfs_fd;    /* дескриптор в VFS (может отличаться от sc_fd) */
    int     flags;     /* O_RDONLY / O_WRONLY / O_RDWR / O_APPEND / ... */
    int     fd_flags;  /* FD_CLOEXEC */
    bool    is_pipe_read;
    bool    is_pipe_write;
    int     pipe_idx;  /* индекс в g_pipes если это pipe */
} sc_file_t;

static sc_file_t g_files[SC_FD_MAX];

/* Аллоцировать sc_fd */
static int sc_alloc_fd(void) {
    /* fd 0, 1, 2 резервируем как stdin/stdout/stderr — они открыты сразу */
    for (int i = 3; i < SC_FD_MAX; i++) {
        if (!g_files[i].open) return i;
    }
    return -1;
}

/* Получить запись по fd */
static sc_file_t *sc_get_file(int fd) {
    if (fd < 0 || fd >= SC_FD_MAX) return NULL;
    if (!g_files[fd].open)         return NULL;
    return &g_files[fd];
}

/* =========================================================================
 * Таблица pipe
 * ========================================================================= */
static pipe_t g_pipes[PIPE_MAX];

static int pipe_alloc(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].valid) {
            memset(&g_pipes[i], 0, sizeof(pipe_t));
            g_pipes[i].valid = true;
            return i;
        }
    }
    return -1;
}

/* =========================================================================
 * Таблица shared memory
 * ========================================================================= */
static shm_region_t g_shm[SHM_MAX_REGIONS];

static int shm_find_by_name(const char *name) {
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (g_shm[i].valid && strcmp(g_shm[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int shm_alloc_slot(void) {
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (!g_shm[i].valid) return i;
    }
    return -1;
}

/* =========================================================================
 * mmap bump-аллокатор
 * (При munmap просто помечаем освобождённым — реального переиспользования нет)
 * ========================================================================= */
static uint64_t g_mmap_next = USER_MMAP_BASE;

static uint64_t mmap_bump_alloc(uint64_t size) {
    uint64_t addr = g_mmap_next;
    if (addr + size > USER_MMAP_LIMIT) return (uint64_t)-1;
    g_mmap_next = ROUND_UP(addr + size, PAGE_SIZE);
    return addr;
}

/* =========================================================================
 * brk (heap)
 * ========================================================================= */
static uint64_t g_brk_current = USER_HEAP_BASE;
static uint64_t g_brk_mapped  = USER_HEAP_BASE;

/* =========================================================================
 * Проверка указателя из userspace
 * ========================================================================= */
bool syscall_validate_ptr(const void *ptr, size_t size) {
    uint64_t va = (uint64_t)(uintptr_t)ptr;
    if (va == 0) return false;
    if (va > USER_VIRT_LIMIT) return false;
    if (size > 0 && (va + size - 1) > USER_VIRT_LIMIT) return false;
    return true;
}

/* Копирует строку из userspace в ядро.
 * Возвращает длину или -1 если не валидный указатель / слишком длинная. */
static int64_t sc_copy_string(const char *user_ptr, char *kbuf, size_t kbuf_size) {
    if (!syscall_validate_ptr(user_ptr, 1)) return -1;
    size_t i = 0;
    const char *p = user_ptr;
    while (i < kbuf_size - 1) {
        /* Страница может быть не маппена — в боевом ядре здесь page fault handling.
         * Пока доверяем что указатель корректный (проверили границу user space). */
        kbuf[i] = p[i];
        if (p[i] == '\0') return (int64_t)i;
        i++;
    }
    kbuf[i] = '\0';
    return (int64_t)i;
}

/* =========================================================================
 * СИСТЕМНЫЕ ВЫЗОВЫ — реализации
 * ========================================================================= */

/* -----------------------------------------------------------------------
 * sys_exit (60) — завершение текущей задачи
 * a1 = код завершения (игнорируем пока нет wait4)
 * ----------------------------------------------------------------------- */
static int64_t sc_exit(uint64_t code) {
    DBG_VAL("SC", "sys_exit code", code);

    task_t *t = sched_current();
    if (t) ui_win_close_all_owned_by((int32_t)t->tid);

    sched_exit();            /* не возвращается */
    __builtin_unreachable();
}

/* -----------------------------------------------------------------------
 * sys_getpid (20/39) — PID текущей задачи
 * ----------------------------------------------------------------------- */
static int64_t sc_getpid(void) {
    task_t *t = sched_current();
    if (!t) return 1;
    return (int64_t)(uint32_t)t->tid;
}

/* -----------------------------------------------------------------------
 * sys_sched_yield (24) — добровольная передача CPU
 * ----------------------------------------------------------------------- */
static int64_t sc_sched_yield(void) {
    sched_yield();
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_fork (57) — упрощённый fork без COW.
 *
 * Полноценный fork требует копирования page tables, дескрипторов, состояния
 * регистров и т.д. Здесь создаём новый поток с тем же точкой входа
 * (в реальности нужен полный контекст).
 *
 * Возвращает: 0 дочернему, tid родителю, -ENOMEM при ошибке.
 *
 * ОГРАНИЧЕНИЕ: текущая реализация возвращает -ENOSYS т.к. полноценный fork
 * требует copy-on-write MMU поддержки которой пока нет.
 * Оставлено как скелет для будущей реализации.
 * ----------------------------------------------------------------------- */
static int64_t sc_fork(void) {
    /*
     * Для полного fork нужно:
     * 1. Аллоцировать новый PML4.
     * 2. Скопировать все user-space PTEs (или пометить COW).
     * 3. Скопировать таблицу fd.
     * 4. Создать новый TCB с копией cpu_frame_t текущего потока.
     * 5. В дочернем — rax=0, в родительском — rax=child_tid.
     *
     * Пока нет per-process PML4 — возвращаем ENOSYS.
     */
    DBG_MSG("SC", "sys_fork: not fully implemented (no per-process PML4)");
    return -(int64_t)ENOSYS;
}

/* -----------------------------------------------------------------------
 * sys_clone (56) — создать kernel-поток.
 *
 * a1 = flags (CLONE_VM | CLONE_FILES | CLONE_THREAD — минимум для потока)
 * a2 = child_stack (указатель на верхушку стека нового потока)
 * a3 = parent_tidptr (игнорируем)
 * a4 = child_tidptr  (игнорируем)
 * a5 = tls           (fs base для нового потока)
 *
 * В текущей реализации потоки разделяют ядерное адресное пространство.
 * Функция потока — сохранённый rip из syscall-entry (rcx), что позволяет
 * запустить POSIX-style pthread_create.
 *
 * Возвращает tid нового потока или отрицательный errno.
 * ----------------------------------------------------------------------- */
typedef struct {
    uint64_t entry_rip;    /* точка входа (user rip при системном вызове clone) */
    uint64_t user_rsp;     /* пользовательский стек */
    uint64_t tls_base;     /* FS base (TLS) */
} clone_arg_t;

static void clone_trampoline(void *arg) {
    clone_arg_t *ca = (clone_arg_t *)arg;
    uint64_t rip = ca->entry_rip;
    uint64_t rsp = ca->user_rsp;
    uint64_t tls = ca->tls_base;
    kfree(ca);

    /* Устанавливаем FS base (TLS) если задан */
    if (tls != 0) {
        __asm__ volatile ("wrfsbase %0" :: "r"(tls));
    }

    /* Прыгаем в ring3 */
    __asm__ volatile (
        "push $0x23\n\t"
        "push %[rsp]\n\t"
        "pushfq\n\t"
        "pop %%rax\n\t"
        "or $0x200, %%rax\n\t"
        "push %%rax\n\t"
        "push $0x1B\n\t"
        "push %[rip]\n\t"
        "xor %%rax, %%rax\n\t"   /* возврат 0 в дочернем потоке */
        "xor %%rbx, %%rbx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%rdi, %%rdi\n\t"
        "xor %%r8,  %%r8\n\t"
        "xor %%r9,  %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        "xor %%r12, %%r12\n\t"
        "xor %%r13, %%r13\n\t"
        "xor %%r14, %%r14\n\t"
        "xor %%r15, %%r15\n\t"
        "xor %%rbp, %%rbp\n\t"
        "iretq\n\t"
        :
        : [rip] "r"(rip), [rsp] "r"(rsp)
        : "rax", "memory"
    );
    __builtin_unreachable();
}

/* user_rip передаётся через сохранённый rcx в syscall_entry (main.c).
 * Вызывающий код в main.c должен передать его как a3 (четвёртый аргумент). */
static int64_t sc_clone(uint64_t flags, uint64_t child_stack,
                        uint64_t user_rip, uint64_t tls) {
    (void)flags; /* TODO: CLONE_VM, CLONE_FILES processing */

    if (child_stack == 0 || !syscall_validate_ptr((void*)child_stack, 8))
        return -(int64_t)EINVAL;
    if (!syscall_validate_ptr((void*)user_rip, 1))
        return -(int64_t)EINVAL;

    clone_arg_t *ca = (clone_arg_t *)kmalloc(sizeof(clone_arg_t));
    if (!ca) return -(int64_t)ENOMEM;
    ca->entry_rip = user_rip;
    ca->user_rsp  = child_stack;
    ca->tls_base  = tls;

    int tid = sched_create_kthread(clone_trampoline, ca, "clone_thread", 0);
    if (tid < 0) {
        kfree(ca);
        return -(int64_t)ENOMEM;
    }
    DBG_VAL("SC", "sys_clone tid", (uint64_t)(uint32_t)tid);
    return (int64_t)(uint32_t)tid;
}

/* -----------------------------------------------------------------------
 * sys_execve (59) — заменить образ текущего процесса.
 *
 * a1 = pathname (char* из user space)
 * a2 = argv[]   (игнорируем пока нет полного argv ABI)
 * a3 = envp[]   (игнорируем)
 *
 * Загружает ELF через elf64_spawn (с новым потоком).
 * ОГРАНИЧЕНИЕ: создаёт новый поток вместо замены текущего.
 * Полный execve требует замены адресного пространства.
 * ----------------------------------------------------------------------- */
static int64_t sc_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr) {
    (void)argv_ptr; (void)envp_ptr;

    char path[VFS_MAX_PATH];
    if (sc_copy_string((const char *)(uintptr_t)path_ptr, path, sizeof(path)) < 0)
        return -(int64_t)EFAULT;

    DBG_MSG("SC", "sys_execve");
    DBG_VAL("SC", "execve path ptr", path_ptr);

    int tid = elf64_spawn(path, "execve", 0);
    if (tid < 0) {
        if (tid == ELF_ERR_IO)    return -(int64_t)ENOENT;
        if (tid == ELF_ERR_NOMEM) return -(int64_t)ENOMEM;
        if (tid == ELF_ERR_MAGIC || tid == ELF_ERR_CLASS ||
            tid == ELF_ERR_ARCH)  return -(int64_t)EACCES;
        return -(int64_t)EINVAL;
    }

    /* Завершаем текущий процесс — управление перейдёт в новый */
    sched_exit();
    __builtin_unreachable();
}

/* -----------------------------------------------------------------------
 * sys_wait4 (61) — ожидание завершения дочернего потока.
 *
 * a1 = tid (или -1 для любого)
 * a2 = *wstatus (указатель на int в user space, куда пишем статус)
 * a3 = options (WNOHANG и т.п. — игнорируем пока)
 *
 * Текущая реализация: поллинг состояния задачи.
 * ZOMBIE-задачи в нашем планировщике помечаются и не возвращаются в очередь.
 * ----------------------------------------------------------------------- */
static int64_t sc_wait4(uint64_t tid_arg, uint64_t wstatus_ptr, uint64_t options) {
    (void)options;

    if (wstatus_ptr != 0 && !syscall_validate_ptr((void*)wstatus_ptr, 4))
        return -(int64_t)EFAULT;

    if (tid_arg == (uint64_t)-1) {
        /* Ждём любого — не реализовано (нет дерева процессов) */
        return -(int64_t)ECHILD;
    }

    uint32_t tid = (uint32_t)tid_arg;

    /* Поллинг: ждём пока задача не станет ZOMBIE */
    uint32_t spin = 0;
    while (1) {
        task_t *t = sched_find(tid);
        if (!t) {
            /* Задача не найдена — уже была уничтожена */
            if (wstatus_ptr)
                *(int32_t *)(uintptr_t)wstatus_ptr = 0;
            return (int64_t)(uint32_t)tid;
        }
        if (t->state == TASK_ZOMBIE) {
            if (wstatus_ptr)
                *(int32_t *)(uintptr_t)wstatus_ptr = 0;
            return (int64_t)(uint32_t)tid;
        }
        /* Уступаем CPU */
        sched_yield();
        spin++;
        if (spin > 100000) return -(int64_t)ECHILD; /* защита от зависания */
    }
}

/* -----------------------------------------------------------------------
 * sys_open (2)
 * a1 = path*, a2 = flags (O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/...)
 * Возвращает sc_fd >= 0 или -errno.
 * ----------------------------------------------------------------------- */
static int64_t sc_open(uint64_t path_ptr, uint64_t flags) {
    char path[VFS_MAX_PATH];
    if (sc_copy_string((const char *)(uintptr_t)path_ptr, path, sizeof(path)) < 0)
        return -(int64_t)EFAULT;

    int vfd = vfs_open(path, (int)flags);
    if (vfd < 0) {
        if (vfd == VFS_ERR_NOENT)  return -(int64_t)ENOENT;
        if (vfd == VFS_ERR_NOMEM)  return -(int64_t)ENOMEM;
        if (vfd == VFS_ERR_NFILE)  return -(int64_t)ENFILE;
        if (vfd == VFS_ERR_ISDIR)  return -(int64_t)EISDIR;
        if (vfd == VFS_ERR_ROFS)   return -(int64_t)EROFS;
        return -(int64_t)EIO;
    }

    int fd = sc_alloc_fd();
    if (fd < 0) {
        vfs_close(vfd);
        return -(int64_t)EMFILE;
    }

    g_files[fd].open           = true;
    g_files[fd].vfs_fd         = vfd;
    g_files[fd].flags          = (int)flags;
    g_files[fd].fd_flags       = 0;
    g_files[fd].is_pipe_read   = false;
    g_files[fd].is_pipe_write  = false;
    g_files[fd].pipe_idx       = -1;

    DBG_VAL("SC", "sys_open fd", (uint64_t)(uint32_t)fd);
    return (int64_t)fd;
}

/* -----------------------------------------------------------------------
 * sys_close (3)
 * ----------------------------------------------------------------------- */
static int64_t sc_close(uint64_t fd_arg) {
    int fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(fd);
    if (!f) return -(int64_t)EBADF;

    if (f->is_pipe_read || f->is_pipe_write) {
        pipe_t *p = &g_pipes[f->pipe_idx];
        if (f->is_pipe_write) p->write_closed = true;
        if (f->is_pipe_read)  p->read_closed  = true;
        /* Если обе стороны закрыты — освобождаем слот */
        if (p->write_closed && p->read_closed) {
            p->valid = false;
        }
    } else {
        vfs_close(f->vfs_fd);
    }

    memset(f, 0, sizeof(sc_file_t));
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_read (0)
 * a1=fd, a2=buf*, a3=count
 * ----------------------------------------------------------------------- */
static int64_t sc_read(uint64_t fd_arg, uint64_t buf_ptr, uint64_t count) {
    if (count == 0) return 0;
    if (!syscall_validate_ptr((void *)(uintptr_t)buf_ptr, (size_t)count))
        return -(int64_t)EFAULT;

    int fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(fd);
    if (!f) return -(int64_t)EBADF;

    /* Pipe read */
    if (f->is_pipe_read) {
        pipe_t *p = &g_pipes[f->pipe_idx];
        if (!p->valid) return -(int64_t)EBADF;

        /* Блокирующий read: ждём данных */
        uint32_t waited = 0;
        while (p->count == 0) {
            if (p->write_closed) return 0;  /* EOF */
            sched_yield();
            waited++;
            if (waited > 1000000) return -(int64_t)EIO;
        }

        uint8_t *buf = (uint8_t *)(uintptr_t)buf_ptr;
        uint64_t n = (count < p->count) ? count : p->count;
        for (uint64_t i = 0; i < n; i++) {
            buf[i] = p->buf[p->read_pos];
            p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        }
        p->count -= (uint32_t)n;
        return (int64_t)n;
    }

    /* Обычный файл */
    if (f->flags == O_WRONLY) return -(int64_t)EBADF;

    int64_t n = vfs_read(f->vfs_fd, (void *)(uintptr_t)buf_ptr, count);
    if (n == VFS_ERR_BADF)    return -(int64_t)EBADF;
    if (n == VFS_ERR_IO)      return -(int64_t)EIO;
    if (n < 0)                return -(int64_t)EIO;
    return n;
}

/* -----------------------------------------------------------------------
 * sys_write (1)
 * a1=fd, a2=buf*, a3=count
 * ----------------------------------------------------------------------- */
static int64_t sc_write(uint64_t fd_arg, uint64_t buf_ptr, uint64_t count) {
    if (count == 0) return 0;
    if (!syscall_validate_ptr((void *)(uintptr_t)buf_ptr, (size_t)count))
        return -(int64_t)EFAULT;

    int fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(fd);
    if (!f) return -(int64_t)EBADF;

    /* Pipe write */
    if (f->is_pipe_write) {
        pipe_t *p = &g_pipes[f->pipe_idx];
        if (!p->valid || p->read_closed) return -(int64_t)EPIPE;

        const uint8_t *buf = (const uint8_t *)(uintptr_t)buf_ptr;
        uint64_t written = 0;
        while (written < count) {
            if (p->count >= PIPE_BUF_SIZE) {
                sched_yield();
                continue;
            }
            p->buf[p->write_pos] = buf[written++];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        return (int64_t)count;
    }

    /* Обычный файл */
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -(int64_t)EBADF;

    int64_t n = vfs_write(f->vfs_fd, (const void *)(uintptr_t)buf_ptr, count);
    if (n == VFS_ERR_ROFS)    return -(int64_t)EROFS;
    if (n == VFS_ERR_NOSPC)   return -(int64_t)ENOSPC;
    if (n < 0)                return -(int64_t)EIO;
    return n;
}

/* -----------------------------------------------------------------------
 * sys_lseek (8)
 * a1=fd, a2=offset, a3=whence (0=SET, 1=CUR, 2=END)
 * ----------------------------------------------------------------------- */
static int64_t sc_lseek(uint64_t fd_arg, uint64_t offset_arg, uint64_t whence) {
    int fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(fd);
    if (!f) return -(int64_t)EBADF;
    if (f->is_pipe_read || f->is_pipe_write) return -(int64_t)EINVAL;

    int64_t offset = (int64_t)offset_arg;
    int64_t r = vfs_seek(f->vfs_fd, offset, (int)whence);
    if (r == VFS_ERR_OVERFLOW) return -(int64_t)EOVERFLOW;
    if (r == VFS_ERR_INVAL)    return -(int64_t)EINVAL;
    if (r < 0)                 return -(int64_t)EINVAL;
    return r;
}

/* -----------------------------------------------------------------------
 * sys_stat (4)
 * a1 = path*, a2 = sc_stat_t*
 * ----------------------------------------------------------------------- */
static int64_t sc_stat(uint64_t path_ptr, uint64_t stat_ptr) {
    if (!syscall_validate_ptr((void *)(uintptr_t)stat_ptr, sizeof(sc_stat_t)))
        return -(int64_t)EFAULT;

    char path[VFS_MAX_PATH];
    if (sc_copy_string((const char *)(uintptr_t)path_ptr, path, sizeof(path)) < 0)
        return -(int64_t)EFAULT;

    vfs_stat_t vs;
    int r = vfs_stat(path, &vs);
    if (r == VFS_ERR_NOENT) return -(int64_t)ENOENT;
    if (r < 0)              return -(int64_t)EIO;

    sc_stat_t *out = (sc_stat_t *)(uintptr_t)stat_ptr;
    out->type       = vs.type;
    out->size       = vs.size;
    out->alloc_size = vs.alloc_size;
    out->ctime      = vs.ctime;
    out->mtime      = vs.mtime;
    out->atime      = vs.atime;
    out->attr       = vs.attr;
    out->cluster    = vs.cluster;
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_fstat (5)
 * a1 = fd, a2 = sc_stat_t*
 * ----------------------------------------------------------------------- */
static int64_t sc_fstat(uint64_t fd_arg, uint64_t stat_ptr) {
    if (!syscall_validate_ptr((void *)(uintptr_t)stat_ptr, sizeof(sc_stat_t)))
        return -(int64_t)EFAULT;

    int fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(fd);
    if (!f || f->is_pipe_read || f->is_pipe_write) return -(int64_t)EBADF;

    vfs_stat_t vs;
    int r = vfs_fstat(f->vfs_fd, &vs);
    if (r < 0) return -(int64_t)EBADF;

    sc_stat_t *out = (sc_stat_t *)(uintptr_t)stat_ptr;
    out->type       = vs.type;
    out->size       = vs.size;
    out->alloc_size = vs.alloc_size;
    out->ctime      = vs.ctime;
    out->mtime      = vs.mtime;
    out->atime      = vs.atime;
    out->attr       = vs.attr;
    out->cluster    = vs.cluster;
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_readdir (89)
 * a1 = fd, a2 = index, a3 = vfs_dirent_t*
 * ----------------------------------------------------------------------- */
static int64_t sc_readdir(uint64_t fd_arg, uint64_t index, uint64_t dirent_ptr) {
    if (!syscall_validate_ptr((void *)(uintptr_t)dirent_ptr, sizeof(vfs_dirent_t)))
        return -(int64_t)EFAULT;

    int fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(fd);
    if (!f || f->is_pipe_read || f->is_pipe_write) return -(int64_t)EBADF;

    int r = vfs_readdir(f->vfs_fd, (uint32_t)index, (vfs_dirent_t *)(uintptr_t)dirent_ptr);
    if (r == VFS_ERR_NOENT)  return -(int64_t)ENOENT;
    if (r == VFS_ERR_NOTDIR) return -(int64_t)ENOTDIR;
    if (r < 0)               return -(int64_t)EIO;
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_mkdir (83)
 * a1 = path*
 * ----------------------------------------------------------------------- */
static int64_t sc_mkdir(uint64_t path_ptr, uint64_t mode) {
    (void)mode;
    char path[VFS_MAX_PATH];
    if (sc_copy_string((const char *)(uintptr_t)path_ptr, path, sizeof(path)) < 0)
        return -(int64_t)EFAULT;

    int r = vfs_mkdir(path);
    if (r == VFS_ERR_EXIST)  return -(int64_t)EEXIST;
    if (r == VFS_ERR_NOENT)  return -(int64_t)ENOENT;
    if (r == VFS_ERR_ROFS)   return -(int64_t)EROFS;
    if (r < 0)               return -(int64_t)EIO;
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_rmdir (84)
 * a1 = path*
 * ----------------------------------------------------------------------- */
static int64_t sc_rmdir(uint64_t path_ptr) {
    char path[VFS_MAX_PATH];
    if (sc_copy_string((const char *)(uintptr_t)path_ptr, path, sizeof(path)) < 0)
        return -(int64_t)EFAULT;

    int r = vfs_rmdir(path);
    if (r == VFS_ERR_NOENT)  return -(int64_t)ENOENT;
    if (r == VFS_ERR_NOTDIR) return -(int64_t)ENOTDIR;
    if (r == VFS_ERR_BUSY)   return -(int64_t)ENOTEMPTY;
    if (r == VFS_ERR_ROFS)   return -(int64_t)EROFS;
    if (r < 0)               return -(int64_t)EIO;
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_unlink (87)
 * a1 = path*
 * ----------------------------------------------------------------------- */
static int64_t sc_unlink(uint64_t path_ptr) {
    char path[VFS_MAX_PATH];
    if (sc_copy_string((const char *)(uintptr_t)path_ptr, path, sizeof(path)) < 0)
        return -(int64_t)EFAULT;

    int r = vfs_unlink(path);
    if (r == VFS_ERR_NOENT)  return -(int64_t)ENOENT;
    if (r == VFS_ERR_ISDIR)  return -(int64_t)EISDIR;
    if (r == VFS_ERR_ROFS)   return -(int64_t)EROFS;
    if (r < 0)               return -(int64_t)EIO;
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_rename (82)
 * a1 = oldpath*, a2 = newpath*
 * (VFS пока не имеет rename — делаем через read+create+unlink)
 * ----------------------------------------------------------------------- */
static int64_t sc_rename(uint64_t old_ptr, uint64_t new_ptr) {
    /*
     * Полноценный rename требует атомарного VFS-примитива.
     * FAT32 драйвер в VFS.h не имеет rename операции.
     * Возвращаем ENOSYS пока не добавим vfs_rename.
     */
    (void)old_ptr; (void)new_ptr;
    DBG_MSG("SC", "sys_rename: ENOSYS (vfs_rename not implemented)");
    return -(int64_t)ENOSYS;
}

/* -----------------------------------------------------------------------
 * sys_chdir (80)
 * a1 = path*
 *
 * VFS пока не поддерживает понятие "текущая директория" глобально,
 * поэтому просто проверяем что путь существует и является директорией.
 * ----------------------------------------------------------------------- */
static char g_cwd[VFS_MAX_PATH] = "/";

static int64_t sc_chdir(uint64_t path_ptr) {
    char path[VFS_MAX_PATH];
    if (sc_copy_string((const char *)(uintptr_t)path_ptr, path, sizeof(path)) < 0)
        return -(int64_t)EFAULT;

    vfs_stat_t vs;
    int r = vfs_stat(path, &vs);
    if (r == VFS_ERR_NOENT)  return -(int64_t)ENOENT;
    if (r < 0)               return -(int64_t)EIO;
    if (vs.type != VFS_TYPE_DIR) return -(int64_t)ENOTDIR;

    strncpy(g_cwd, path, VFS_MAX_PATH - 1);
    g_cwd[VFS_MAX_PATH - 1] = '\0';
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_ftruncate (91)
 * a1 = fd, a2 = length
 * ----------------------------------------------------------------------- */
static int64_t sc_ftruncate(uint64_t fd_arg, uint64_t length) {
    int fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(fd);
    if (!f || f->is_pipe_read || f->is_pipe_write) return -(int64_t)EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -(int64_t)EBADF;

    int r = vfs_ftruncate(f->vfs_fd, length);
    if (r == VFS_ERR_ROFS)  return -(int64_t)EROFS;
    if (r == VFS_ERR_NOSPC) return -(int64_t)ENOSPC;
    if (r < 0)              return -(int64_t)EIO;
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_fcntl (72)
 * a1 = fd, a2 = cmd, a3 = arg
 * ----------------------------------------------------------------------- */
static int64_t sc_fcntl(uint64_t fd_arg, uint64_t cmd, uint64_t arg) {
    int fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(fd);
    if (!f) return -(int64_t)EBADF;

    switch ((int)cmd) {
    case F_DUPFD: {
        /* Дублируем fd: найти свободный fd >= arg */
        int new_fd = -1;
        for (int i = (int)arg; i < SC_FD_MAX; i++) {
            if (!g_files[i].open) { new_fd = i; break; }
        }
        if (new_fd < 0) return -(int64_t)EMFILE;
        g_files[new_fd] = *f;
        g_files[new_fd].fd_flags = 0;  /* FD_CLOEXEC не наследуется при dup */
        return (int64_t)new_fd;
    }
    case F_GETFD:
        return (int64_t)f->fd_flags;
    case F_SETFD:
        f->fd_flags = (int)(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL:
        return (int64_t)f->flags;
    case F_SETFL:
        /* Разрешаем менять только O_APPEND, O_NONBLOCK (пока игнорируем нелинейные) */
        f->flags = (f->flags & O_ACCMODE) | ((int)arg & ~(int)O_ACCMODE);
        return 0;
    default:
        return -(int64_t)EINVAL;
    }
}

/* -----------------------------------------------------------------------
 * sys_brk (12)
 * a1 = new_brk (0 = вернуть текущий)
 * ----------------------------------------------------------------------- */
static int64_t sc_brk(uint64_t new_brk) {
    if (new_brk == 0)
        return (int64_t)g_brk_current;

    if (new_brk < USER_HEAP_BASE)
        return (int64_t)g_brk_current;  /* нельзя уменьшить ниже базы */
    if (new_brk > USER_HEAP_MAX)
        return -(int64_t)ENOMEM;

    /* Расширяем: маппим новые страницы */
    if (new_brk > g_brk_mapped) {
        uint64_t map_start = ROUND_UP(g_brk_mapped, PAGE_SIZE);
        uint64_t map_end   = ROUND_UP(new_brk, PAGE_SIZE);
        uint64_t npages    = (map_end - map_start) >> PAGE_SHIFT;

        if (npages > 0) {
            uint64_t pa = pmm_alloc_frames((size_t)npages, 1);
            if (pa == (uint64_t)-1) return -(int64_t)ENOMEM;

            /* Обнуляем (BSS-стиль) */
            uint8_t *kp = (uint8_t *)(uintptr_t)(pa + DIRECT_MAP_OFFSET);
            memset(kp, 0, (size_t)(npages << PAGE_SHIFT));

            for (uint64_t i = 0; i < npages; i++) {
                int r = vmm_map_user(map_start + (i << PAGE_SHIFT),
                                     pa + (i << PAGE_SHIFT), true, false);
                if (r != 0) {
                    pmm_free_frames(pa, (size_t)npages);
                    return -(int64_t)ENOMEM;
                }
            }
            g_brk_mapped = map_end;
        }
    }

    g_brk_current = new_brk;
    return (int64_t)new_brk;
}

/* -----------------------------------------------------------------------
 * sys_mmap (9)
 * a1 = addr (hint), a2 = length, a3 = prot,
 * a4 = flags, a5 = fd (для file-backed), a6 = offset
 *
 * Поддерживается:
 *   MAP_ANON  — анонимный регион, обнуляется
 *   MAP_PRIVATE | MAP_FIXED — маппинг по конкретному адресу
 * File-backed: только для O_RDONLY (read-only copy из файла)
 * ----------------------------------------------------------------------- */
static int64_t sc_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                       uint64_t flags, uint64_t fd_arg, uint64_t offset) {
    if (length == 0) return -(int64_t)EINVAL;

    uint64_t npages = (ROUND_UP(length, PAGE_SIZE)) >> PAGE_SHIFT;
    bool rw   = (prot & PROT_WRITE) != 0;
    bool exec = (prot & PROT_EXEC)  != 0;

    /* Выбираем виртуальный адрес */
    uint64_t va;
    if ((flags & MAP_FIXED) && addr != 0) {
        va = ROUND_DOWN(addr, PAGE_SIZE);
        if (va > USER_VIRT_LIMIT) return -(int64_t)EINVAL;
    } else {
        va = mmap_bump_alloc(npages << PAGE_SHIFT);
        if (va == (uint64_t)-1) return -(int64_t)ENOMEM;
    }

    if (flags & MAP_ANONYMOUS) {
        /* Анонимный маппинг — аллоцируем физические страницы и обнуляем */
        uint64_t pa = pmm_alloc_frames((size_t)npages, 1);
        if (pa == (uint64_t)-1) return -(int64_t)ENOMEM;

        uint8_t *kp = (uint8_t *)(uintptr_t)(pa + DIRECT_MAP_OFFSET);
        memset(kp, 0, (size_t)(npages << PAGE_SHIFT));

        for (uint64_t i = 0; i < npages; i++) {
            int r = vmm_map_user(va + (i << PAGE_SHIFT),
                                 pa + (i << PAGE_SHIFT), rw, exec);
            if (r != 0) {
                pmm_free_frames(pa, (size_t)npages);
                return -(int64_t)ENOMEM;
            }
        }
        return (int64_t)va;
    }

    /* File-backed mmap */
    int sc_fd = (int)(uint32_t)fd_arg;
    sc_file_t *f = sc_get_file(sc_fd);
    if (!f || f->is_pipe_read || f->is_pipe_write) return -(int64_t)EBADF;

    /* Аллоцируем физические страницы */
    uint64_t pa = pmm_alloc_frames((size_t)npages, 1);
    if (pa == (uint64_t)-1) return -(int64_t)ENOMEM;

    /* Обнуляем (BSS/padding) */
    uint8_t *kbuf = (uint8_t *)(uintptr_t)(pa + DIRECT_MAP_OFFSET);
    memset(kbuf, 0, (size_t)(npages << PAGE_SHIFT));

    /* Читаем из файла */
    int64_t seek_r = vfs_seek(f->vfs_fd, (int64_t)offset, 0);
    if (seek_r < 0) {
        pmm_free_frames(pa, (size_t)npages);
        return -(int64_t)EINVAL;
    }

    int64_t nr = vfs_read(f->vfs_fd, kbuf, (uint64_t)(npages << PAGE_SHIFT));
    if (nr < 0) {
        pmm_free_frames(pa, (size_t)npages);
        return -(int64_t)EIO;
    }

    /* Маппим */
    for (uint64_t i = 0; i < npages; i++) {
        int r = vmm_map_user(va + (i << PAGE_SHIFT),
                             pa + (i << PAGE_SHIFT), rw, exec);
        if (r != 0) {
            pmm_free_frames(pa, (size_t)npages);
            return -(int64_t)ENOMEM;
        }
    }
    return (int64_t)va;
}

/* -----------------------------------------------------------------------
 * sys_munmap (11)
 * a1 = addr, a2 = length
 *
 * Размаппирует страницы. Физическую память не освобождаем (нет tracking),
 * только убираем PTEs — реальная ОС держит vm_area_struct.
 * ----------------------------------------------------------------------- */
static int64_t sc_munmap(uint64_t addr, uint64_t length) {
    if (length == 0) return -(int64_t)EINVAL;
    if (addr > USER_VIRT_LIMIT) return -(int64_t)EINVAL;

    uint64_t va    = ROUND_DOWN(addr, PAGE_SIZE);
    uint64_t npages = ROUND_UP(length, PAGE_SIZE) >> PAGE_SHIFT;

    for (uint64_t i = 0; i < npages; i++) {
        vmm_unmap(va + (i << PAGE_SHIFT));
        /* Игнорируем ошибку — страница могла не быть маппена */
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_mprotect (10)
 * a1 = addr, a2 = length, a3 = prot
 *
 * Изменяет права доступа к страницам.
 * В текущей VMM нет отдельной функции change_prot, поэтому делаем
 * через vmm_unmap + vmm_map с новыми флагами.
 * ОГРАНИЧЕНИЕ: теряем оригинальный PA если не знаем текущий маппинг.
 * Полная реализация требует vma_list (vm_area_struct).
 * ----------------------------------------------------------------------- */
static int64_t sc_mprotect(uint64_t addr, uint64_t length, uint64_t prot) {
    /*
     * Без vma tracking мы не можем безопасно изменить права страниц
     * не зная их физический адрес. Для будущей полноценной реализации
     * нужен список vm_area_struct per process.
     * Пока возвращаем 0 (успех) если аргументы корректны, чтобы не ломать
     * приложения которые вызывают mprotect для совместимости.
     */
    if (addr > USER_VIRT_LIMIT) return -(int64_t)EINVAL;
    if (length == 0)            return -(int64_t)EINVAL;
    (void)prot;

    DBG_MSG("SC", "sys_mprotect: stub (no vma tracking)");
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_arch_prctl (158)
 * a1 = code (ARCH_SET_FS / ARCH_GET_FS / ...)
 * a2 = addr
 * ----------------------------------------------------------------------- */
static int64_t sc_arch_prctl(uint64_t code, uint64_t addr) {
    switch (code) {
    case ARCH_SET_FS:
        /* wrfsbase — разрешено в ring3 если CR4.FSGSBASE установлен.
         * Мы устанавливаем его в usermode_init через CR4 |= (1<<16). */
        __asm__ volatile ("wrfsbase %0" :: "r"(addr));
        return 0;
    case ARCH_GET_FS: {
        if (!syscall_validate_ptr((void *)(uintptr_t)addr, 8))
            return -(int64_t)EFAULT;
        uint64_t fs;
        __asm__ volatile ("rdfsbase %0" : "=r"(fs));
        *(uint64_t *)(uintptr_t)addr = fs;
        return 0;
    }
    case ARCH_SET_GS:
        __asm__ volatile ("wrgsbase %0" :: "r"(addr));
        return 0;
    case ARCH_GET_GS: {
        if (!syscall_validate_ptr((void *)(uintptr_t)addr, 8))
            return -(int64_t)EFAULT;
        uint64_t gs;
        __asm__ volatile ("rdgsbase %0" : "=r"(gs));
        *(uint64_t *)(uintptr_t)addr = gs;
        return 0;
    }
    default:
        return -(int64_t)EINVAL;
    }
}

/* -----------------------------------------------------------------------
 * sys_pipe (502)
 * a1 = int[2]* (pipefd[0]=read, pipefd[1]=write)
 * ----------------------------------------------------------------------- */
static int64_t sc_pipe(uint64_t pipefd_ptr) {
    if (!syscall_validate_ptr((void *)(uintptr_t)pipefd_ptr, 8))
        return -(int64_t)EFAULT;

    int pidx = pipe_alloc();
    if (pidx < 0) return -(int64_t)ENFILE;

    int fd_r = sc_alloc_fd();
    if (fd_r < 0) {
        g_pipes[pidx].valid = false;
        return -(int64_t)EMFILE;
    }

    int fd_w = sc_alloc_fd();
    if (fd_w < 0) {
        g_pipes[pidx].valid = false;
        memset(&g_files[fd_r], 0, sizeof(sc_file_t));
        return -(int64_t)EMFILE;
    }

    /* Читающий конец */
    g_files[fd_r].open          = true;
    g_files[fd_r].vfs_fd        = -1;
    g_files[fd_r].flags         = O_RDONLY;
    g_files[fd_r].is_pipe_read  = true;
    g_files[fd_r].is_pipe_write = false;
    g_files[fd_r].pipe_idx      = pidx;

    /* Пишущий конец */
    g_files[fd_w].open          = true;
    g_files[fd_w].vfs_fd        = -1;
    g_files[fd_w].flags         = O_WRONLY;
    g_files[fd_w].is_pipe_read  = false;
    g_files[fd_w].is_pipe_write = true;
    g_files[fd_w].pipe_idx      = pidx;

    int32_t *pfd = (int32_t *)(uintptr_t)pipefd_ptr;
    pfd[0] = fd_r;
    pfd[1] = fd_w;

    DBG_VAL("SC", "sys_pipe fd_r", (uint64_t)(uint32_t)fd_r);
    DBG_VAL("SC", "sys_pipe fd_w", (uint64_t)(uint32_t)fd_w);
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_shm_get (503)
 * a1 = name* (char*), a2 = size, a3 = flags (0=get existing, 1=create)
 * Возвращает индекс региона (>= 0) или -errno.
 * ----------------------------------------------------------------------- */
static int64_t sc_shm_get(uint64_t name_ptr, uint64_t size, uint64_t flags) {
    char name[SHM_MAX_NAME];
    if (sc_copy_string((const char *)(uintptr_t)name_ptr, name, sizeof(name)) < 0)
        return -(int64_t)EFAULT;

    /* Ищем существующий */
    int idx = shm_find_by_name(name);
    if (idx >= 0) {
        g_shm[idx].refcount++;
        return (int64_t)idx;
    }

    /* Нет и не создаём */
    if (!(flags & 1)) return -(int64_t)ENOENT;

    /* Создаём новый */
    if (size == 0 || size > 64ULL * 1024 * 1024) return -(int64_t)EINVAL;

    idx = shm_alloc_slot();
    if (idx < 0) return -(int64_t)ENFILE;

    uint64_t npages = ROUND_UP(size, PAGE_SIZE) >> PAGE_SHIFT;
    uint64_t pa = pmm_alloc_frames((size_t)npages, 1);
    if (pa == (uint64_t)-1) return -(int64_t)ENOMEM;

    /* Обнуляем через direct map */
    uint8_t *kp = (uint8_t *)(uintptr_t)(pa + DIRECT_MAP_OFFSET);
    memset(kp, 0, (size_t)(npages << PAGE_SHIFT));

    strncpy(g_shm[idx].name, name, SHM_MAX_NAME - 1);
    g_shm[idx].name[SHM_MAX_NAME - 1] = '\0';
    g_shm[idx].phys_base    = pa;
    g_shm[idx].virt_kernel  = pa + DIRECT_MAP_OFFSET;
    g_shm[idx].size         = size;
    g_shm[idx].pages        = (uint32_t)npages;
    g_shm[idx].refcount     = 1;
    g_shm[idx].valid        = true;

    DBG_VAL("SC", "sys_shm_get idx", (uint64_t)(uint32_t)idx);
    return (int64_t)idx;
}

/* -----------------------------------------------------------------------
 * sys_shm_at (504)
 * a1 = shm_idx, a2 = addr_hint (0 = ядро выбирает), a3 = prot
 * Возвращает user virtual address или -errno.
 * ----------------------------------------------------------------------- */
static int64_t sc_shm_at(uint64_t shm_idx_arg, uint64_t addr_hint, uint64_t prot) {
    int idx = (int)(uint32_t)shm_idx_arg;
    if (idx < 0 || idx >= SHM_MAX_REGIONS || !g_shm[idx].valid)
        return -(int64_t)EINVAL;

    shm_region_t *shm = &g_shm[idx];
    bool rw   = (prot & PROT_WRITE) != 0;
    bool exec = (prot & PROT_EXEC)  != 0;

    /* Выбираем VA */
    uint64_t va;
    if (addr_hint != 0 && addr_hint <= USER_VIRT_LIMIT) {
        va = ROUND_DOWN(addr_hint, PAGE_SIZE);
    } else {
        va = mmap_bump_alloc(shm->pages << PAGE_SHIFT);
        if (va == (uint64_t)-1) return -(int64_t)ENOMEM;
    }

    /* Маппим страницы */
    for (uint32_t i = 0; i < shm->pages; i++) {
        int r = vmm_map_user(va + ((uint64_t)i << PAGE_SHIFT),
                             shm->phys_base + ((uint64_t)i << PAGE_SHIFT),
                             rw, exec);
        if (r != 0) return -(int64_t)ENOMEM;
    }

    DBG_VAL("SC", "sys_shm_at va", va);
    return (int64_t)va;
}

/* -----------------------------------------------------------------------
 * sys_shm_detach (505)
 * a1 = user virtual address (возвращённый shm_at)
 * a2 = shm_idx
 *
 * Убирает PTEs, уменьшает refcount, освобождает если 0.
 * ----------------------------------------------------------------------- */
static int64_t sc_shm_detach(uint64_t va, uint64_t shm_idx_arg) {
    int idx = (int)(uint32_t)shm_idx_arg;
    if (idx < 0 || idx >= SHM_MAX_REGIONS || !g_shm[idx].valid)
        return -(int64_t)EINVAL;

    shm_region_t *shm = &g_shm[idx];

    /* Убираем PTEs */
    for (uint32_t i = 0; i < shm->pages; i++) {
        vmm_unmap(ROUND_DOWN(va, PAGE_SIZE) + ((uint64_t)i << PAGE_SHIFT));
    }

    shm->refcount--;
    if (shm->refcount == 0) {
        pmm_free_frames(shm->phys_base, (size_t)shm->pages);
        memset(shm, 0, sizeof(shm_region_t));
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_get_framebuffer (500)
 *
 * Возвращает реальный GOP linear framebuffer (см. Framebuffer.c: g_fb),
 * если экран инициализирован в FB_MODE_LINEAR — то есть настоящие
 * width/height/pitch/bpp и физический адрес буфера. Именно это нужно
 * userspace-приложениям (в т.ч. будущему полноценному My Computer.elf),
 * чтобы рисовать напрямую в кадр через собственный mmap физического
 * адреса (sc_mmap с MAP_PHYS/аналогом — см. память ниже).
 *
 * Если экран не в linear-режиме (например ещё не инициализирован, или
 * GOP недоступен) — отдаём VGA text buffer (0xB8000, 80x25) как и раньше,
 * чтобы вызывающий код (например ранний init) не падал.
 *
 * a1 = sc_framebuffer_t* (из user space)
 * ----------------------------------------------------------------------- */
static int64_t sc_get_framebuffer(uint64_t fb_ptr) {
    if (!syscall_validate_ptr((void *)(uintptr_t)fb_ptr, sizeof(sc_framebuffer_t)))
        return -(int64_t)EFAULT;

    sc_framebuffer_t *fb = (sc_framebuffer_t *)(uintptr_t)fb_ptr;

    if (fb_get_mode() == FB_MODE_LINEAR_ && fb_phys_addr() != 0) {
        fb->phys_addr = fb_phys_addr();
        /* virt_addr — DIRECT_MAP-адрес того же физического буфера в
         * адресном пространстве ЯДРА; userspace всё равно должен сам
         * замаппить fb->phys_addr к себе через sc_mmap (физический
         * маппинг), поэтому здесь достаточно продублировать phys_addr —
         * значение поля "для справки", не для прямого разыменования
         * из userspace. */
        fb->virt_addr = fb_phys_addr();
        fb->width     = fb_width();
        fb->height    = fb_height();
        fb->pitch     = fb_pitch();
        fb->bpp       = fb_bpp();
        fb->mode      = 1;   /* 1 = linear GOP framebuffer */

        DBG_MSG("SC", "sys_get_framebuffer: linear GOP");
        return 0;
    }

    fb->phys_addr = VGA_TEXT_PHYS;
    fb->virt_addr = VGA_TEXT_PHYS;  /* у нас identity-маппинг для VGA через DIRECT_MAP */
    fb->width     = VGA_TEXT_COLS;
    fb->height    = VGA_TEXT_ROWS;
    fb->pitch     = VGA_TEXT_COLS * 2;  /* 2 байта на символ */
    fb->bpp       = 4;                  /* VGA text: 4 bpp (2 байта: char + attr) */
    fb->mode      = 0;                  /* 0 = VGA text mode 80x25 */

    DBG_MSG("SC", "sys_get_framebuffer: VGA fallback (linear FB не готов)");
    return 0;
}

/* -----------------------------------------------------------------------
 * Оконные системные вызовы — тонкая обёртка над генерическим оконным
 * менеджером из Framebuffer.c (ui_win_create/ui_win_close/ui_win_move).
 * Это тот же самый код, которым ядро пользуется для своих встроенных
 * окон (например "My Computer" — см. Framebuffer.c), так что любое
 * userspace-приложение открывает окно и получает кнопку на панели
 * задач ровно так же, как встроенные приложения ядра.
 *
 * sys_win_create (a1 = title_ptr, a2 = title_len, a3 = w, a4 = h)
 *   Возвращает win_id (>=0) или -EFAULT/-EINVAL.
 * sys_win_close  (a1 = win_id)
 * sys_win_move   (a1 = win_id, a2 = x, a3 = y)
 * ----------------------------------------------------------------------- */
#define UI_WIN_TITLE_MAX_SC   64

static int64_t sc_win_create(uint64_t title_ptr, uint64_t title_len,
                             uint64_t w, uint64_t h) {
    if (title_len == 0 || title_len >= UI_WIN_TITLE_MAX_SC)
        title_len = UI_WIN_TITLE_MAX_SC - 1;

    if (!syscall_validate_ptr((void *)(uintptr_t)title_ptr, (size_t)title_len))
        return -(int64_t)EFAULT;

    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        return -(int64_t)EINVAL;

    char title[UI_WIN_TITLE_MAX_SC];
    const char *src = (const char *)(uintptr_t)title_ptr;
    size_t n = 0;
    while (n < title_len && src[n] != '\0') { title[n] = src[n]; n++; }
    title[n] = '\0';

    task_t *cur = sched_current();
    int32_t owner_pid = cur ? (int32_t)cur->tid : -1;

    int id = ui_win_create(title, (uint32_t)w, (uint32_t)h, owner_pid);
    if (id < 0) {
        DBG_MSG("SC", "sys_win_create: не удалось создать окно (нет слотов?)");
        return -(int64_t)ENOMEM;
    }

    DBG_VAL("SC", "sys_win_create: id", (uint64_t)id);
    return (int64_t)id;
}

static int64_t sc_win_close(uint64_t win_id) {
    if (ui_win_close((int)win_id) != 0)
        return -(int64_t)EINVAL;
    return 0;
}

static int64_t sc_win_move(uint64_t win_id, uint64_t x, uint64_t y) {
    if (ui_win_move((int)win_id, (int32_t)x, (int32_t)y) != 0)
        return -(int64_t)EINVAL;
    return 0;
}

/* =========================================================================
 * Главный диспетчер
 * ========================================================================= */
int64_t syscall_dispatch(uint64_t nr,
                         uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
    DBG_VAL("SC", "syscall nr", nr);

    switch (nr) {
    /* --- I/O --- */
    case SYS_READ:          return sc_read(a1, a2, a3);
    case SYS_WRITE:         return sc_write(a1, a2, a3);
    case SYS_OPEN:          return sc_open(a1, a2);
    case SYS_CLOSE:         return sc_close(a1);
    case SYS_STAT:          return sc_stat(a1, a2);
    case SYS_FSTAT:         return sc_fstat(a1, a2);
    case SYS_LSEEK:         return sc_lseek(a1, a2, a3);
    case SYS_READDIR:       return sc_readdir(a1, a2, a3);
    case SYS_FTRUNCATE:     return sc_ftruncate(a1, a2);
    case SYS_FCNTL:         return sc_fcntl(a1, a2, a3);
    case SYS_MKDIR:         return sc_mkdir(a1, a2);
    case SYS_RMDIR:         return sc_rmdir(a1);
    case SYS_UNLINK:        return sc_unlink(a1);
    case SYS_RENAME:        return sc_rename(a1, a2);
    case SYS_CHDIR:         return sc_chdir(a1);
    /* --- Память --- */
    case SYS_BRK:           return sc_brk(a1);
    case SYS_MMAP:          return sc_mmap(a1, a2, a3, a4, a5, 0);
    case SYS_MUNMAP:        return sc_munmap(a1, a2);
    case SYS_MPROTECT:      return sc_mprotect(a1, a2, a3);
    /* --- Процессы --- */
    case SYS_EXIT:          return sc_exit(a1);
    case SYS_FORK:          return sc_fork();
    case SYS_CLONE:         return sc_clone(a1, a2, a3, a4);
    case SYS_EXECVE:        return sc_execve(a1, a2, a3);
    case SYS_WAIT4:         return sc_wait4(a1, a2, a3);
    case SYS_GETPID:
    case SYS_GETPID2:       return sc_getpid();
    case SYS_SCHED_YIELD:   return sc_sched_yield();
    /* --- Прочее --- */
    case SYS_ARCH_PRCTL:    return sc_arch_prctl(a1, a2);
    case SYS_PIPE:          return sc_pipe(a1);
    case SYS_SHM_GET:       return sc_shm_get(a1, a2, a3);
    case SYS_SHM_AT:        return sc_shm_at(a1, a2, a3);
    case SYS_SHM_DETACH:    return sc_shm_detach(a1, a2);
    /* --- Графика --- */
    case SYS_GET_FRAMEBUFFER: return sc_get_framebuffer(a1);
    case SYS_WIN_CREATE:      return sc_win_create(a1, a2, a3, a4);
    case SYS_WIN_CLOSE:       return sc_win_close(a1);
    case SYS_WIN_MOVE:        return sc_win_move(a1, a2, a3);
    default:
        DBG_VAL("SC", "unknown syscall", nr);
        return -(int64_t)ENOSYS;
    }
}

/* =========================================================================
 * Инициализация
 * ========================================================================= */
void syscall_init(void) {
    memset(g_files, 0, sizeof(g_files));
    memset(g_pipes, 0, sizeof(g_pipes));
    memset(g_shm,   0, sizeof(g_shm));

    /* Инициализируем stdin/stdout/stderr как "открытые" заглушки.
     * Реальный write на fd=1/2 идёт через sc_write → VGA/COM1.
     * Пока VGA драйвер не является VFS-узлом, stdout не поддерживается
     * через vfs_write — это будет реализовано в следующей итерации. */
    g_files[0].open  = true; g_files[0].vfs_fd = -1; g_files[0].flags = O_RDONLY;
    g_files[1].open  = true; g_files[1].vfs_fd = -1; g_files[1].flags = O_WRONLY;
    g_files[2].open  = true; g_files[2].vfs_fd = -1; g_files[2].flags = O_WRONLY;

    g_brk_current = USER_HEAP_BASE;
    g_brk_mapped  = USER_HEAP_BASE;
    g_mmap_next   = USER_MMAP_BASE;

    DBG_MSG("SC", "syscall_init ok");
}

#ifdef __cplusplus
}
#endif
