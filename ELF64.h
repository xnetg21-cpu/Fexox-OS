/*
 * ELF64.h — ELF64 parser & loader for FEXOS
 *
 * Поддерживает:
 *   - статические PIE и non-PIE ELF64 исполняемые файлы (ET_EXEC, ET_DYN)
 *   - загрузку сегментов PT_LOAD в user address space
 *   - начальный user stack (argv/envp/auxv ABI compatible)
 *   - запуск через sched_create_kthread-обёртку (user entry вызывается из ring3)
 *
 * Зависимости ядра которые используются:
 *   MemoryControl: kmalloc/kfree, pmm_alloc_frames/pmm_free_frames,
 *                  vmm_map_user (va, pa, rw, exec)
 *   VFS:           vfs_open/vfs_read/vfs_seek/vfs_fstat/vfs_close
 *   Scheduler:     sched_create_kthread (для запуска user-процесса)
 *   debug_out.h:   DBG_MSG / DBG_VAL / DBG_PANIC
 */

#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * ELF64 типы (SYSV ABI)
 * ========================================================================= */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

/* ---- ELF identity ---- */
#define EI_NIDENT   16
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define EI_OSABI    7

#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

#define ELFCLASS64  2   /* 64-bit */
#define ELFDATA2LSB 1   /* little-endian */
#define EV_CURRENT  1

/* ---- e_type ---- */
#define ET_EXEC     2   /* исполняемый */
#define ET_DYN      3   /* shared object / PIE */

/* ---- e_machine ---- */
#define EM_X86_64   62

/* ---- p_type ---- */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_PHDR     6
#define PT_TLS      7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

/* ---- p_flags ---- */
#define PF_X        0x1   /* execute */
#define PF_W        0x2   /* write */
#define PF_R        0x4   /* read */

/* =========================================================================
 * ELF64 заголовки
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint8_t     e_ident[EI_NIDENT];
    Elf64_Half  e_type;
    Elf64_Half  e_machine;
    Elf64_Word  e_version;
    Elf64_Addr  e_entry;        /* точка входа */
    Elf64_Off   e_phoff;        /* смещение program header table */
    Elf64_Off   e_shoff;        /* смещение section header table */
    Elf64_Word  e_flags;
    Elf64_Half  e_ehsize;       /* размер этого заголовка (64) */
    Elf64_Half  e_phentsize;    /* размер одной записи PHT */
    Elf64_Half  e_phnum;        /* кол-во записей PHT */
    Elf64_Half  e_shentsize;
    Elf64_Half  e_shnum;
    Elf64_Half  e_shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;       /* смещение в файле */
    Elf64_Addr  p_vaddr;        /* виртуальный адрес в памяти */
    Elf64_Addr  p_paddr;        /* физический (игнорируем) */
    Elf64_Xword p_filesz;       /* размер в файле */
    Elf64_Xword p_memsz;        /* размер в памяти (memsz >= filesz, хвост = BSS) */
    Elf64_Xword p_align;        /* выравнивание (степень 2, 0 или 1 = нет) */
} Elf64_Phdr;

/* =========================================================================
 * Коды ошибок elf64_load
 * ========================================================================= */
#define ELF_OK          0
#define ELF_ERR_IO      (-1)   /* ошибка VFS */
#define ELF_ERR_MAGIC   (-2)   /* не ELF */
#define ELF_ERR_CLASS   (-3)   /* не 64-bit */
#define ELF_ERR_ENDIAN  (-4)   /* не LE */
#define ELF_ERR_TYPE    (-5)   /* не ET_EXEC / ET_DYN */
#define ELF_ERR_ARCH    (-6)   /* не x86_64 */
#define ELF_ERR_NOMEM   (-7)   /* нет памяти */
#define ELF_ERR_INVAL   (-8)   /* битый ELF */
#define ELF_ERR_INTERP  (-9)   /* требует динамический линкер (не поддерживаем) */

/* =========================================================================
 * Результат загрузки
 * ========================================================================= */
#define ELF64_MAX_SEGS  16     /* максимум PT_LOAD сегментов на один ELF */

typedef struct {
    Elf64_Addr  vaddr;         /* виртуальный адрес начала сегмента (выровнен) */
    uint64_t    num_pages;     /* кол-во страниц */
    uint64_t    phys_base;     /* первый физический фрейм */
    bool        exec;
    bool        write;
} elf64_seg_t;

typedef struct {
    Elf64_Addr  entry;         /* точка входа (с учётом PIE-базы) */
    Elf64_Addr  load_base;     /* база загрузки (0 для ET_EXEC, random для PIE) */
    Elf64_Addr  stack_top;     /* вершина user stack (RSP при старте) */
    uint64_t    stack_pages;   /* кол-во страниц стека */
    uint64_t    stack_phys;    /* физический адрес стека (для освобождения) */

    uint32_t    nseg;
    elf64_seg_t segs[ELF64_MAX_SEGS];
} elf64_image_t;

/* =========================================================================
 * Публичный API
 * ========================================================================= */

/*
 * elf64_load — загружает ELF64 из VFS-файла в user address space.
 *
 *   path      — путь в VFS (e.g. "/init")
 *   out       — заполняется при успехе
 *
 * Возвращает ELF_OK или отрицательный код ошибки.
 *
 * Аллоцирует физические страницы через pmm_alloc_frames и
 * маппит их в user-space через vmm_map_user.
 * BSS-часть сегмента (memsz > filesz) обнуляется.
 */
int elf64_load(const char *path, elf64_image_t *out);

/*
 * elf64_free — освобождает физические страницы загруженного образа.
 * Вызывать когда процесс завершён и его страницы больше не нужны.
 * Виртуальные маппинги должны быть убраны до вызова (или при уничтожении PML4).
 */
void elf64_free(elf64_image_t *img);

/*
 * elf64_spawn — удобная обёртка: загружает ELF и создаёт kernel-поток,
 * который прыгает в user mode через iret (ring 3).
 *
 *   path      — путь к ELF-файлу в VFS
 *   name      — имя потока для планировщика
 *   prio      — приоритет (SCHED_PRIO_NORMAL = 0)
 *
 * Возвращает tid >= 0 или отрицательный код ошибки.
 *
 * Примечание: текущая реализация не передаёт argv/envp (argc=0).
 * Расширение — добавить параметры позже.
 */
int elf64_spawn(const char *path, const char *name, int prio);

/*
 * elf64_validate_header — только валидирует заголовок (не загружает).
 * Полезно для проверки перед загрузкой.
 */
int elf64_validate_header(const Elf64_Ehdr *ehdr);

#endif /* ELF64_H */
