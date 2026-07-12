/*
 * fxapp.c — загрузчик манифестов приложений FEXOS (.fxapp)
 *
 * Зависимости: VFS (vfs_open/vfs_read/vfs_close), klibc (memset),
 * ELF64 (elf64_spawn — вызывается только из fxapp_launch), debug_out.h.
 *
 * Как и остальные модули ядра, файл сам объявляет нужные extern'ы —
 * в проекте нет общего заголовка для VFS/ELF64 (см. комментарии в
 * PS2Mouse.c/ui_extra.c).
 */
#include <stdint.h>
#include <stddef.h>
#include "fxapp.h"
#include "debug_out.h"

extern void *memset(void *dst, int c, size_t n);

extern int     vfs_open(const char *path, int flags);
extern int     vfs_close(int fd);
extern int64_t vfs_read(int fd, void *buf, uint64_t size);

extern int elf64_spawn(const char *path, const char *name, int prio);

#ifndef FXAPP_O_RDONLY
#define FXAPP_O_RDONLY 0
#endif

int fxapp_load(const char *path, fxapp_t *out) {
    if (!path || !out) return -1;

    memset(out, 0, sizeof(*out));

    int fd = vfs_open(path, FXAPP_O_RDONLY);
    if (fd < 0) {
        DBG_MSG("FXA", "fxapp_load: файл не найден");
        return -1;
    }

    int64_t got = vfs_read(fd, &out->header, sizeof(out->header));
    vfs_close(fd);

    if (got != (int64_t)sizeof(out->header)) {
        DBG_MSG("FXA", "fxapp_load: файл слишком мал/оборван");
        return -1;
    }

    if (out->header.magic != FXAPP_MAGIC) {
        DBG_MSG("FXA", "fxapp_load: неверная магия (не .fxapp?)");
        return -1;
    }

    if (out->header.version != FXAPP_VERSION) {
        DBG_VAL("FXA", "fxapp_load: неизвестная версия", (uint64_t)out->header.version);
        return -1;
    }

    /* Сейчас формат поддерживает только приложения на СИ (статический
     * ELF64, тот же путь загрузки, что и /init — см. ELF64.c). Любой
     * другой app_type отклоняем, чтобы не пытаться "запустить" то,
     * что ядро не умеет исполнять. */
    if (out->header.app_type != FXAPP_TYPE_C_ELF64) {
        DBG_VAL("FXA", "fxapp_load: неподдерживаемый app_type", (uint64_t)out->header.app_type);
        return -1;
    }

    /* Гарантируем нуль-терминацию строковых полей независимо от
     * содержимого файла на диске (не доверяем внешним данным). */
    out->header.name[FXAPP_NAME_MAX - 1] = '\0';
    out->header.icon[FXAPP_ICON_MAX - 1] = '\0';
    out->header.elf_path[FXAPP_PATH_MAX - 1] = '\0';

    out->valid = 1;
    DBG_MSG("FXA", "fxapp_load: OK");
    return 0;
}

int fxapp_launch(const fxapp_t *app, int prio) {
    if (!app || !app->valid) return -1;
    if (app->header.app_type != FXAPP_TYPE_C_ELF64) return -1;
    if (app->header.elf_path[0] == '\0') {
        /* У приложения ещё нет исполняемого кода (например у "My
         * Computer" сейчас есть только окно-заглушка) — это не ошибка
         * дескриптора, просто запускать нечего. */
        DBG_MSG("FXA", "fxapp_launch: elf_path пуст, запускать нечего");
        return -1;
    }

    return elf64_spawn(app->header.elf_path, app->header.name, prio);
}
