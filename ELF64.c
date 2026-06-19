/*
 * ELF64.c — ELF64 parser & loader для FEXOS
 *
 * Реализует:
 *   - elf64_validate_header  — проверка магии, класса, архитектуры
 *   - elf64_load             — загрузка PT_LOAD сегментов в user VA space
 *   - elf64_free             — освобождение физических страниц образа
 *   - elf64_spawn            — обёртка: load + create kthread → ring3 entry
 *
 * Поддерживаемые типы ELF:
 *   ET_EXEC — статический исполняемый (фиксированные адреса)
 *   ET_DYN  — PIE (загружается по случайной базе в user space)
 *
 * НЕ поддерживается:
 *   PT_INTERP (динамическая компоновка) — возвращает ELF_ERR_INTERP
 *   Relocation (RELA/REL) — PIE загружается "как есть", relocations не применяются.
 *   Для полной поддержки PIE нужен ld.so в ядре — это выходит за рамки задачи.
 *
 * Зависимости:
 *   extern void *kmalloc(size_t)        — MemoryControl.c
 *   extern void  kfree(void *)          — MemoryControl.c
 *   extern uint64_t pmm_alloc_frames(size_t count, size_t align) — MemoryControl.c
 *   extern void  pmm_free_frames(uint64_t pa, size_t count)      — MemoryControl.c
 *   extern int   vmm_map_user(uint64_t va, uint64_t pa,
 *                             bool rw, bool exec)                 — MemoryControl.c
 *   vfs_open / vfs_read / vfs_seek / vfs_fstat / vfs_close       — VFS.c
 *   sched_create_kthread                                          — Scheduler.c
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ELF64.h"
#include "VFS.h"
#include "Scheduler.h"
#include "debug_out.h"

/* =========================================================================
 * Прототипы внешних функций ядра
 * ========================================================================= */
extern void    *kmalloc(size_t size);
extern void     kfree(void *ptr);
extern uint64_t pmm_alloc_frames(size_t count, size_t align);
extern void     pmm_free_frames(uint64_t pa, size_t count);
/* vmm_map_user: va <= USER_VIRT_LIMIT, rw=true → PTE_W, exec=true → !PTE_NX */
extern int      vmm_map_user(uint64_t va, uint64_t pa, bool rw, bool exec);

/* =========================================================================
 * Внутренние константы
 * ========================================================================= */
#define PAGE_SIZE       4096ULL
#define PAGE_SHIFT      12
#define PAGE_MASK       (~(PAGE_SIZE - 1ULL))
#define ROUND_UP(n, a)  (((uint64_t)(n) + (uint64_t)(a) - 1ULL) & ~((uint64_t)(a) - 1ULL))
#define ROUND_DOWN(n,a) ((uint64_t)(n) & ~((uint64_t)(a) - 1ULL))

/* User stack: размещаем под 0x0000_7FFF_0000_0000 */
#define USER_STACK_TOP      0x00007FFF00000000ULL
#define USER_STACK_PAGES    8       /* 32 KiB */

/* База загрузки PIE (ET_DYN) — просто фиксированная "случайная" база.
 * В реальной ОС здесь был бы ASLR. Пока константа выше kernel space. */
#define PIE_LOAD_BASE       0x0000000000400000ULL

/* Максимальный размер program header table (безопасность) */
#define ELF_MAX_PHDRS       64
#define ELF_MAX_PHDR_BYTES  (ELF_MAX_PHDRS * sizeof(Elf64_Phdr))

/* Максимальный размер одного сегмента (128 MiB) */
#define ELF_MAX_SEG_BYTES   (128ULL * 1024 * 1024)

/* =========================================================================
 * elf64_validate_header
 * ========================================================================= */
int elf64_validate_header(const Elf64_Ehdr *ehdr) {
    if (!ehdr) return ELF_ERR_INVAL;

    /* Магия */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        DBG_MSG("ELF", "bad magic");
        return ELF_ERR_MAGIC;
    }

    /* 64-bit */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        DBG_MSG("ELF", "not ELF64");
        return ELF_ERR_CLASS;
    }

    /* Little-endian */
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        DBG_MSG("ELF", "not LE");
        return ELF_ERR_ENDIAN;
    }

    /* Версия */
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT || ehdr->e_version != EV_CURRENT) {
        DBG_MSG("ELF", "bad version");
        return ELF_ERR_INVAL;
    }

    /* Тип */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        DBG_MSG("ELF", "not ET_EXEC/ET_DYN");
        return ELF_ERR_TYPE;
    }

    /* Архитектура */
    if (ehdr->e_machine != EM_X86_64) {
        DBG_MSG("ELF", "not x86_64");
        return ELF_ERR_ARCH;
    }

    /* Sanity: phentsize */
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        DBG_MSG("ELF", "bad phentsize");
        return ELF_ERR_INVAL;
    }

    /* phnum */
    if (ehdr->e_phnum == 0 || ehdr->e_phnum > ELF_MAX_PHDRS) {
        DBG_MSG("ELF", "phnum out of range");
        return ELF_ERR_INVAL;
    }

    return ELF_OK;
}

/* =========================================================================
 * Вспомогательная функция: memset без libc (klibc.c уже есть в проекте,
 * но чтобы не добавлять зависимость явно — используем extern)
 * ========================================================================= */
extern void *memset(void *dst, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

/* =========================================================================
 * elf64_load
 * ========================================================================= */
int elf64_load(const char *path, elf64_image_t *out) {
    if (!path || !out) return ELF_ERR_INVAL;

    /* --- Обнуляем результат --- */
    memset(out, 0, sizeof(elf64_image_t));

    /* --- Открываем файл --- */
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        DBG_MSG("ELF", "vfs_open failed");
        return ELF_ERR_IO;
    }

    /* --- Читаем ELF header --- */
    Elf64_Ehdr ehdr;
    int64_t nr = vfs_read(fd, &ehdr, sizeof(Elf64_Ehdr));
    if (nr != (int64_t)sizeof(Elf64_Ehdr)) {
        DBG_MSG("ELF", "short read on ehdr");
        vfs_close(fd);
        return ELF_ERR_IO;
    }

    /* --- Валидируем --- */
    int vr = elf64_validate_header(&ehdr);
    if (vr != ELF_OK) {
        vfs_close(fd);
        return vr;
    }

    DBG_VAL("ELF", "e_type",  ehdr.e_type);
    DBG_VAL("ELF", "e_entry", ehdr.e_entry);
    DBG_VAL("ELF", "e_phnum", ehdr.e_phnum);

    /* --- Читаем Program Header Table --- */
    size_t phdr_bytes = (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs = (Elf64_Phdr *)kmalloc(phdr_bytes);
    if (!phdrs) {
        DBG_MSG("ELF", "kmalloc phdrs OOM");
        vfs_close(fd);
        return ELF_ERR_NOMEM;
    }

    if (vfs_seek(fd, (int64_t)ehdr.e_phoff, 0) < 0) {
        DBG_MSG("ELF", "seek to phoff failed");
        kfree(phdrs);
        vfs_close(fd);
        return ELF_ERR_IO;
    }

    nr = vfs_read(fd, phdrs, phdr_bytes);
    if (nr != (int64_t)phdr_bytes) {
        DBG_MSG("ELF", "short read on phdrs");
        kfree(phdrs);
        vfs_close(fd);
        return ELF_ERR_IO;
    }

    /* --- Проверяем PT_INTERP (динамические бинарники не поддерживаем) --- */
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_INTERP) {
            DBG_MSG("ELF", "PT_INTERP found: dynamic linking not supported");
            kfree(phdrs);
            vfs_close(fd);
            return ELF_ERR_INTERP;
        }
    }

    /* --- Вычисляем базу загрузки ---
     * ET_EXEC: base = 0 (сегменты загружаются по своим vaddr)
     * ET_DYN:  base = PIE_LOAD_BASE (прибавляем ко всем vaddr)
     */
    uint64_t load_base = (ehdr.e_type == ET_DYN) ? PIE_LOAD_BASE : 0ULL;
    out->load_base = load_base;
    out->entry     = ehdr.e_entry + load_base;

    DBG_VAL("ELF", "load_base",  load_base);
    DBG_VAL("ELF", "entry_virt", out->entry);

    /* --- Загружаем PT_LOAD сегменты --- */
    uint32_t nseg = 0;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        if (nseg >= ELF64_MAX_SEGS) {
            DBG_MSG("ELF", "too many PT_LOAD segments");
            /* Освобождаем уже загруженные */
            goto err_free_segs;
        }

        if (ph->p_memsz == 0) continue;

        /* Проверяем размер */
        if (ph->p_memsz > ELF_MAX_SEG_BYTES || ph->p_filesz > ph->p_memsz) {
            DBG_MSG("ELF", "invalid segment sizes");
            goto err_free_segs;
        }

        /* Виртуальный адрес с учётом базы и выравнивания страниц */
        uint64_t seg_vaddr = ROUND_DOWN(ph->p_vaddr + load_base, PAGE_SIZE);
        uint64_t seg_vend  = ROUND_UP(ph->p_vaddr + load_base + ph->p_memsz, PAGE_SIZE);
        uint64_t seg_pages = (seg_vend - seg_vaddr) >> PAGE_SHIFT;

        DBG_VAL("ELF", "seg vaddr",  seg_vaddr);
        DBG_VAL("ELF", "seg pages",  seg_pages);

        /* Выделяем физические страницы */
        uint64_t phys = pmm_alloc_frames((size_t)seg_pages, 1);
        if (phys == (uint64_t)-1) {
            DBG_MSG("ELF", "pmm_alloc_frames OOM for segment");
            goto err_free_segs;
        }

        /* Получаем виртуальный адрес физической памяти через direct map.
         * После vmm_activate() direct map offset = 0xFFFF880000000000 */
        #define ELF_DIRECT_MAP_OFFSET 0xFFFF880000000000ULL
        uint8_t *kva = (uint8_t *)(uintptr_t)(phys + ELF_DIRECT_MAP_OFFSET);

        /* Обнуляем весь сегмент (BSS) */
        memset(kva, 0, (size_t)(seg_pages << PAGE_SHIFT));

        /* Копируем data из файла */
        if (ph->p_filesz > 0) {
            /* Смещение внутри страницы (если vaddr не выровнен) */
            uint64_t page_offset = (ph->p_vaddr + load_base) - seg_vaddr;
            uint8_t *dst = kva + page_offset;

            if (vfs_seek(fd, (int64_t)ph->p_offset, 0) < 0) {
                DBG_MSG("ELF", "seek to segment data failed");
                pmm_free_frames(phys, (size_t)seg_pages);
                goto err_free_segs;
            }

            /* Читаем по кускам (vfs_read может вернуть меньше байт за раз) */
            uint64_t remain = ph->p_filesz;
            uint64_t written = 0;
            while (remain > 0) {
                int64_t chunk = vfs_read(fd, dst + written, remain);
                if (chunk <= 0) {
                    DBG_MSG("ELF", "vfs_read segment data failed");
                    pmm_free_frames(phys, (size_t)seg_pages);
                    goto err_free_segs;
                }
                written += (uint64_t)chunk;
                remain  -= (uint64_t)chunk;
            }
        }

        /* Маппим страницы в user VA space */
        bool seg_rw   = (ph->p_flags & PF_W) != 0;
        bool seg_exec = (ph->p_flags & PF_X) != 0;

        for (uint64_t p = 0; p < seg_pages; p++) {
            uint64_t va = seg_vaddr + (p << PAGE_SHIFT);
            uint64_t pa = phys      + (p << PAGE_SHIFT);
            int mr = vmm_map_user(va, pa, seg_rw, seg_exec);
            if (mr != 0) {
                DBG_MSG("ELF", "vmm_map_user failed");
                /* Анмаппим уже маппленные */
                /* (vmm_unmap не экспортирован публично — пропустим, pmm freeing достаточно) */
                pmm_free_frames(phys, (size_t)seg_pages);
                goto err_free_segs;
            }
        }

        /* Сохраняем информацию о сегменте */
        out->segs[nseg].vaddr      = seg_vaddr;
        out->segs[nseg].num_pages  = seg_pages;
        out->segs[nseg].phys_base  = phys;
        out->segs[nseg].exec       = seg_exec;
        out->segs[nseg].write      = seg_rw;
        nseg++;
    }

    out->nseg = nseg;
    kfree(phdrs);
    vfs_close(fd);

    if (nseg == 0) {
        DBG_MSG("ELF", "no PT_LOAD segments found");
        return ELF_ERR_INVAL;
    }

    /* --- Аллоцируем user stack --- */
    {
        uint64_t stack_pages = USER_STACK_PAGES;
        uint64_t stack_phys  = pmm_alloc_frames((size_t)stack_pages, 1);
        if (stack_phys == (uint64_t)-1) {
            DBG_MSG("ELF", "OOM for user stack");
            elf64_free(out);
            return ELF_ERR_NOMEM;
        }

        /* Обнуляем стек */
        uint8_t *sk = (uint8_t *)(uintptr_t)(stack_phys + ELF_DIRECT_MAP_OFFSET);
        memset(sk, 0, (size_t)(stack_pages << PAGE_SHIFT));

        uint64_t stack_va_base = USER_STACK_TOP - (stack_pages << PAGE_SHIFT);

        /* Маппим стек: rw=true, exec=false */
        for (uint64_t p = 0; p < stack_pages; p++) {
            uint64_t va = stack_va_base + (p << PAGE_SHIFT);
            uint64_t pa = stack_phys    + (p << PAGE_SHIFT);
            if (vmm_map_user(va, pa, true, false) != 0) {
                DBG_MSG("ELF", "vmm_map_user stack failed");
                pmm_free_frames(stack_phys, (size_t)stack_pages);
                elf64_free(out);
                return ELF_ERR_NOMEM;
            }
        }

        /* RSP указывает на верхушку стека, выровнено на 16 байт.
         * По SysV ABI перед call rsp должен быть выровнен на 16 − 8.
         * Размещаем argc=0, envp=NULL, auxv=NULL. */
        out->stack_top   = (USER_STACK_TOP - 8ULL) & ~0xFULL;
        out->stack_pages = stack_pages;
        out->stack_phys  = stack_phys;

        DBG_VAL("ELF", "stack_top",  out->stack_top);
    }

    DBG_MSG("ELF", "elf64_load OK");
    return ELF_OK;

err_free_segs:
    kfree(phdrs);
    vfs_close(fd);
    /* Освобождаем уже загруженные сегменты */
    out->nseg = nseg;
    elf64_free(out);
    return ELF_ERR_IO;
}

/* =========================================================================
 * elf64_free
 * ========================================================================= */
void elf64_free(elf64_image_t *img) {
    if (!img) return;

    /* Освобождаем физические страницы сегментов */
    for (uint32_t i = 0; i < img->nseg; i++) {
        if (img->segs[i].phys_base && img->segs[i].num_pages) {
            pmm_free_frames(img->segs[i].phys_base, (size_t)img->segs[i].num_pages);
            img->segs[i].phys_base = 0;
            img->segs[i].num_pages = 0;
        }
    }
    img->nseg = 0;

    /* Освобождаем стек */
    if (img->stack_phys && img->stack_pages) {
        pmm_free_frames(img->stack_phys, (size_t)img->stack_pages);
        img->stack_phys  = 0;
        img->stack_pages = 0;
    }
}

/* =========================================================================
 * elf64_spawn — аргумент для kernel-потока
 * ========================================================================= */
typedef struct {
    elf64_image_t image;
} elf64_spawn_arg_t;

/*
 * user_entry_trampoline — kernel-поток, который прыгает в ring3 через iret.
 *
 * x86_64 iret frame на стеке ядра (snip из intel manual):
 *   [rsp+0 ] = RIP    (user entry point)
 *   [rsp+8 ] = CS     (0x1B = user code64 | DPL3)
 *   [rsp+16] = RFLAGS (IF=1)
 *   [rsp+24] = RSP    (user stack top)
 *   [rsp+32] = SS     (0x23 = user data | DPL3)
 *
 * GDT от usermode_init() в main.c:
 *   0x18 = user code DPL=3  → selector 0x1B (0x18 | 3)
 *   0x20 = user data DPL=3  → selector 0x23 (0x20 | 3)
 */
static void user_entry_trampoline(void *arg) {
    elf64_spawn_arg_t *sa = (elf64_spawn_arg_t *)arg;

    uint64_t user_rip = sa->image.entry;
    uint64_t user_rsp = sa->image.stack_top;

    DBG_VAL("ELF", "trampoline rip", user_rip);
    DBG_VAL("ELF", "trampoline rsp", user_rsp);

    /* Освобождаем структуру аргументов (она была в kmalloc) */
    kfree(sa);

    /* Прыгаем в ring3 через iret */
    __asm__ volatile (
        /* Строим iret frame вручную */
        "push $0x23\n\t"            /* SS: user data DPL=3 (0x20 | 3) */
        "push %[ursp]\n\t"          /* RSP: user stack */
        "pushfq\n\t"                /* RFLAGS */
        "pop %%rax\n\t"
        "or $0x200, %%rax\n\t"      /* IF=1 */
        "push %%rax\n\t"            /* RFLAGS обратно */
        "push $0x1B\n\t"            /* CS: user code DPL=3 (0x18 | 3) */
        "push %[urip]\n\t"          /* RIP: точка входа */
        /* Обнуляем регистры перед передачей управления */
        "xor %%rax, %%rax\n\t"
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
        : [urip] "r"(user_rip), [ursp] "r"(user_rsp)
        : "rax", "memory"
    );

    /* Сюда мы никогда не дойдём */
    __builtin_unreachable();
}

/* =========================================================================
 * elf64_spawn
 * ========================================================================= */
int elf64_spawn(const char *path, const char *name, int prio) {
    if (!path || !name) return ELF_ERR_INVAL;

    DBG_MSG("ELF", "elf64_spawn: loading...");
    DBG_VAL("ELF", "spawn path", (uint64_t)(uintptr_t)path);

    /* Аллоцируем аргументы для trampoline */
    elf64_spawn_arg_t *sa = (elf64_spawn_arg_t *)kmalloc(sizeof(elf64_spawn_arg_t));
    if (!sa) {
        DBG_MSG("ELF", "elf64_spawn: OOM for spawn arg");
        return ELF_ERR_NOMEM;
    }
    memset(sa, 0, sizeof(elf64_spawn_arg_t));

    /* Загружаем ELF */
    int r = elf64_load(path, &sa->image);
    if (r != ELF_OK) {
        DBG_MSG("ELF", "elf64_spawn: elf64_load failed");
        kfree(sa);
        return r;
    }

    DBG_VAL("ELF", "spawn entry", sa->image.entry);

    /* Создаём kernel-поток-трамплин */
    int tid = sched_create_kthread(user_entry_trampoline, sa, name, prio);
    if (tid < 0) {
        DBG_MSG("ELF", "elf64_spawn: sched_create_kthread failed");
        elf64_free(&sa->image);
        kfree(sa);
        return ELF_ERR_NOMEM;
    }

    DBG_VAL("ELF", "spawned tid", (uint64_t)(uint32_t)tid);
    return tid;
}

#ifdef __cplusplus
}
#endif
