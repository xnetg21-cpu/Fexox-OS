#ifndef FXAPP_H
#define FXAPP_H

#include <stdint.h>

/*
 * fxapp.h — формат приложений FEXOS (.fxapp)
 *
 * .fxapp — это НЕ исполняемый файл сам по себе, а лёгкий "манифест"
 * приложения: имя, иконка и путь к настоящему бинарнику. Пока формат
 * поддерживает только один тип приложений — статические ELF64 (те же,
 * что грузит ELF64.c/elf64_spawn), поэтому FXAPP_TYPE_C_ELF64 сейчас
 * единственное валидное значение app_type.
 *
 * Расположение на диске: обычно APPS/<Имя>.fxapp (см. FAT32/VFS).
 * Иконка лежит в UI/<icon> (тот же каталог, что и cursor.png/обои).
 *
 * Пример дескриптора для "My Computer":
 *   name       = "My Computer"
 *   icon       = "MyPC.png"
 *   elf_path   = "APPS/mycomputer.elf"   (может быть пустым, пока
 *                                          интерфейс приложения не готов)
 *
 * fxapp_load() читает и валидирует файл через VFS. fxapp_launch()
 * запускает elf_path через elf64_spawn() — используется только когда
 * у приложения реально есть исполняемый код; для чисто "оконных"
 * заглушек (как My Computer сейчас) вызывать необязательно.
 */

#define FXAPP_MAGIC        0x50415846u   /* "FXAP" (little-endian) */
#define FXAPP_VERSION      1

/* Единственный поддерживаемый на сегодня тип — статический ELF64,
 * запускаемый как обычный процесс через elf64_spawn(). Другие типы
 * (скрипты, байткод и т.п.) не поддерживаются и загрузка с ними
 * должна быть отклонена. */
#define FXAPP_TYPE_C_ELF64 1

#define FXAPP_NAME_MAX     64
#define FXAPP_ICON_MAX      64
#define FXAPP_PATH_MAX      96

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             /* FXAPP_MAGIC */
    uint16_t version;           /* FXAPP_VERSION */
    uint16_t app_type;          /* FXAPP_TYPE_* */
    char     name[FXAPP_NAME_MAX];      /* отображаемое имя, напр. "My Computer" */
    char     icon[FXAPP_ICON_MAX];      /* имя файла иконки внутри UI/, напр. "MyPC.png" */
    char     elf_path[FXAPP_PATH_MAX];  /* путь к ELF64-бинарнику (VFS), может быть "" */
    uint32_t icon_w, icon_h;    /* ожидаемый размер иконки в пикселях (0 = не задан) */
    uint8_t  reserved[64];      /* на будущее (аргументы, флаги и т.п.) */
} fxapp_header_t;
#pragma pack(pop)

typedef struct {
    fxapp_header_t header;
    int             valid;      /* 1, если header успешно загружен и провалидирован */
} fxapp_t;

/* fxapp_load — читает <path> через VFS и заполняет *out.
 * Возвращает 0 при успехе, -1 при ошибке (файл не найден, битая
 * магия/версия, неизвестный app_type). */
int fxapp_load(const char *path, fxapp_t *out);

/* fxapp_launch — запускает приложение из уже загруженного дескриптора
 * через elf64_spawn(header.elf_path, header.name, prio).
 * Возвращает tid (>=0) при успехе, отрицательное значение при ошибке
 * (в т.ч. если elf_path пуст — у приложения ещё нет исполняемого кода,
 * как сейчас у "My Computer": оно только показывает окно). */
int fxapp_launch(const fxapp_t *app, int prio);

#endif /* FXAPP_H */
