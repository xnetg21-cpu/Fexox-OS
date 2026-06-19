/*
 * klibc.c — минимальные замены libc для freestanding-ядра FEXOS
 *
 * Эти функции нужны потому что компилятор может генерировать их вызовы
 * при: копировании структур (memcpy), инициализации (memset), и т.д.
 * В freestanding-окружении libc нет, поэтому реализуем сами.
 */
#include <stdint.h>
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

/* Сравнение строк. Возвращает <0, 0 или >0. */
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Сравнение не более n символов. */
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Копирование строки в буфер размером n.
 * Всегда завершает dst нулём (в отличие от strncpy из libc).
 * Возвращает dst. */
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    /* Заполняем остаток нулями — поведение стандартного strncpy */
    while (i < n) dst[i++] = '\0';
    return dst;
}

/* Безопасное копирование: гарантирует \0 даже если src длиннее n-1.
 * Возвращает длину src (можно проверить усечение: ret >= n).
 * Аналог strlcpy из BSD. */
size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t src_len = strlen(src);
    if (n > 0) {
        size_t copy = (src_len < n - 1) ? src_len : n - 1;
        /* memcpy вместо цикла — компилятор может оптимизировать */
        const char *s = src;
        char       *d = dst;
        size_t      c = copy;
        while (c--) *d++ = *s++;
        *d = '\0';
    }
    return src_len;
}

/* Поиск символа в строке. Возвращает указатель или NULL. */
char *strchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    while (*s) {
        if ((unsigned char)*s == ch) return (char *)s;
        s++;
    }
    return (ch == '\0') ? (char *)s : (void *)0;
}

/* Поиск последнего вхождения символа. */
char *strrchr(const char *s, int c) {
    unsigned char  ch  = (unsigned char)c;
    const char    *last = (void *)0;
    while (*s) {
        if ((unsigned char)*s == ch) last = s;
        s++;
    }
    if (ch == '\0') return (char *)s;
    return (char *)last;
}

/* Конкатенация не более n символов из src в dst.
 * dst должен быть достаточно большим. Возвращает dst. */
char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}
