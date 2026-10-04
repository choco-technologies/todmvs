#include "private.h"
#include <stdarg.h>
#include <string.h>

/*
 * Text built in memory - the view is written once it is complete (its
 * declarations come before the code that uses them, and are known only at
 * the end), and to a temporary file renamed over the output, so a failed
 * conversion leaves no half-written view.
 */

#define FORMAT_MAX      256u

void text_free(text_t* t)
{
    Dmod_Free(t->data);
    memset(t, 0, sizeof(*t));
}

void text_add(text_t* t, const char* s, size_t length)
{
    if (t->failed)
        return;
    if (t->size + length + 1U > t->capacity)
    {
        size_t capacity = (t->capacity == 0) ? 4096U : t->capacity;
        while (t->size + length + 1U > capacity)
            capacity *= 2U;
        char* data = Dmod_Malloc(capacity);
        if (data == NULL)
        {
            t->failed = true;
            return;
        }
        if (t->size != 0)
            memcpy(data, t->data, t->size);
        Dmod_Free(t->data);
        t->data = data;
        t->capacity = capacity;
    }
    memcpy(t->data + t->size, s, length);
    t->size += length;
    t->data[t->size] = '\0';
}

void text_str(text_t* t, const char* s)
{
    text_add(t, s, strlen(s));
}

void text_fmt(text_t* t, const char* format, ...)
{
    char buffer[FORMAT_MAX];
    va_list args;
    va_start(args, format);
    int n = Dmod_VSnPrintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0)
        text_add(t, buffer, ((size_t)n < sizeof(buffer)) ? (size_t)n : sizeof(buffer) - 1U);
}

void text_color(text_t* t, uint32_t color)
{
    static const char digits[] = "0123456789ABCDEF";
    char s[10];
    uint32_t n = ((color >> 24) == 0xFFu) ? 6U : 8U;
    s[0] = '#';
    for (uint32_t i = 0; i < n; i++)
        s[1 + i] = digits[(color >> (4U * (n - 1U - i))) & 0xFu];
    text_add(t, s, n + 1U);
}

void text_string(text_t* t, const char* s, size_t length)
{
    static const char digits[] = "0123456789ABCDEF";
    const char* end = s + length;
    text_add(t, "\"", 1);
    while (s < end)
    {
        uint8_t c = (uint8_t)*s;
        if (c == '"' || c == '\\')
        {
            char e[2] = { '\\', (char)c };
            text_add(t, e, 2);
            s++;
            continue;
        }
        if (c < 0x20u || c == 0x7Fu)
        {
            char e[4] = { '\\', 'x', digits[c >> 4], digits[c & 0xFu] };
            text_add(t, e, 4);
            s++;
            continue;
        }
        /* A character of the private use area (icons) as \x escapes - an
         * editor shows nothing of it */
        const char* start = s;
        uint32_t cp = (c >= 0x80u) ? dmvsi_utf8_next(&s, end) : (uint32_t)*s++;
        if (cp >= 0xE000u && cp <= 0xF8FFu)
        {
            for (const char* p = start; p < s; p++)
            {
                uint8_t b = (uint8_t)*p;
                char e[4] = { '\\', 'x', digits[b >> 4], digits[b & 0xFu] };
                text_add(t, e, 4);
            }
        }
        else
            text_add(t, start, (size_t)(s - start));
    }
    text_add(t, "\"", 1);
}

/* Written to "<path>.<pid>-<id>.tmp" - unique for every running conversion:
 * the pid, and the address of a local of the call - and renamed */
bool text_save(const text_t* t, const char* path)
{
    size_t size = strlen(path) + 32U;
    char* temp = Dmod_Malloc(size);
    if (t->failed || temp == NULL)
    {
        Dmod_Free(temp);
        return false;
    }
    Dmod_SnPrintf(temp, size, "%s.%x-%x.tmp", path, (unsigned)Dmod_GetCurrentPid(), (unsigned)(uintptr_t)&size);
    void* f = Dmod_FileOpen(temp, "wb");
    bool ok = f != NULL && (t->size == 0 || Dmod_FileWrite(t->data, 1, t->size, f) == t->size);
    if (f != NULL)
        Dmod_FileClose(f);

    /* The old file first: not every file system's rename replaces a file */
    if (ok && Dmod_FileAvailable(path))
        (void)Dmod_FileRemove(path);
    ok = ok && Dmod_Rename(temp, path) == 0;
    if (!ok && f != NULL)
        (void)Dmod_FileRemove(temp);
    Dmod_Free(temp);
    return ok;
}

bool text_load(text_t* t, const char* path)
{
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return false;
    char buffer[512];
    size_t n;
    while ((n = Dmod_FileRead(buffer, 1, sizeof(buffer), f)) > 0)
        text_add(t, buffer, n);
    Dmod_FileClose(f);
    return !t->failed;
}
