#include "private.h"
#include <errno.h>
#include <string.h>
#include <stddef.h>

/*
 * What a view needs next to it: its fonts as dmod's assets (a copy of the
 * font file and its .ini - a section per size the view uses, with only its
 * characters), and its images.
 *
 * An .ini that is in the output directory already - made for another view
 * - keeps its sections: a section of the same name (the same font, size and
 * letter spacing) gets the characters of both.
 *
 * An image's .ini has a section per size (and blur) the views draw it at -
 * dmod makes a .dmvi of each; one drawn at its own size is a section with
 * no keys.
 */

#define MAX_SECTIONS        64u
#define MAX_NAME            48u
#define MAX_CHARS_LINE      4096u
#define COPY_BUFFER         1024u

const char* base_name(const char* path)
{
    const char* slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

void dir_of(const char* path, char* dir, size_t size)
{
    const char* slash = strrchr(path, '/');
    size_t n = (slash != NULL) ? (size_t)(slash - path) : 0;
    if (slash == path)
        n = 1;                                  /* "/view.dmvs": the root */
    if (n + 1U > size)
        n = size - 1U;
    if (n == 0)
    {
        strcpy(dir, ".");
        return;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
}

/* "inter-bold-48", "inter-bold-48-t-0.55"; the built-in font "sans-16" */
void font_spec(dmvsi_font_t font, char* spec, size_t size)
{
    dmvsi_font_info_t info;
    char base[MAX_NAME];
    size_t n = 0;
    (void)dmvsi_font_info(font, &info);
    if (info.file == NULL)
    {
        Dmod_SnPrintf(spec, size, "sans-%u", (unsigned)info.size);
        return;
    }
    const char* name = base_name(info.file);
    const char* dot = strrchr(name, '.');
    const char* end = (dot != NULL && dot != name) ? dot : name + strlen(name);
    /* The base no longer than leaves room for "-<size>-t-<tracking>" in spec (a long file name cut, not its size) */
    size_t room = (size > 16U) ? size - 16U : 1U;
    if (room > sizeof(base))
        room = sizeof(base);
    for (const char* p = name; p < end && n + 1U < room; p++)
    {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            c = '-';
        if (c == '-' && (n == 0 || base[n - 1U] == '-'))
            continue;
        base[n++] = c;
    }
    while (n > 0 && base[n - 1U] == '-')
        n--;
    base[n] = '\0';
    if (n == 0)
        strcpy(base, "font");
    if (end - name >= (ptrdiff_t)room && room > 28U)
    {
        /* Cut: its beginning and a hash of all of it (fonts.gstatic.com's names differ at their ends) */
        uint32_t h = 2166136261u;
        for (const char* p = name; p < end; p++)
            h = (h ^ (uint8_t)*p) * 16777619u;
        n = (n > room - 9U - 1U) ? room - 9U - 1U : n;
        while (n > 0 && base[n - 1U] == '-')
            n--;
        Dmod_SnPrintf(base + n, sizeof(base) - n, "-%06x", (unsigned)(h & 0xFFFFFFu));
    }

    if (info.tracking == 0)
    {
        Dmod_SnPrintf(spec, size, "%s-%u", base, (unsigned)info.size);
        return;
    }
    int32_t t = (info.tracking < 0) ? -info.tracking : info.tracking;
    char fraction[4] = { (char)('0' + (t % 100) / 10), (char)('0' + t % 10), '\0', '\0' };
    if (fraction[1] == '0')
        fraction[1] = '\0';
    if (t % 100 == 0)
        Dmod_SnPrintf(spec, size, "%s-%u-t%s%d", base, (unsigned)info.size, (info.tracking < 0) ? "-" : "", (int)(t / 100));
    else
        Dmod_SnPrintf(spec, size, "%s-%u-t%s%d.%s", base, (unsigned)info.size, (info.tracking < 0) ? "-" : "", (int)(t / 100),
                      fraction);
}

/* ---- Copies ---- */

int copy_file(const char* from, const char* to)
{
    if (strcmp(from, to) == 0)
        return 0;
    void* in = Dmod_FileOpen(from, "rb");
    if (in == NULL)
        return -ENOENT;
    void* out = Dmod_FileOpen(to, "wb");
    if (out == NULL)
    {
        Dmod_FileClose(in);
        return -EIO;
    }
    char* buffer = Dmod_Malloc(COPY_BUFFER);
    bool ok = buffer != NULL;
    size_t n;
    while (ok && (n = Dmod_FileRead(buffer, 1, COPY_BUFFER, in)) > 0)
        ok = Dmod_FileWrite(buffer, 1, n, out) == n;
    Dmod_Free(buffer);
    Dmod_FileClose(in);
    Dmod_FileClose(out);
    return ok ? 0 : (buffer == NULL) ? -ENOMEM : -EIO;
}

/* ---- .ini ---- */

typedef struct
{
    char        name[MAX_NAME];
    uint32_t    size;
    int32_t     tracking;           /* 1/100 pixel */
    uint32_t*   chars;
    uint32_t    count;
    uint32_t    capacity;
} section_t;

typedef struct
{
    section_t   sections[MAX_SECTIONS];
    uint32_t    count;
    bool        failed;
} ini_t;

static void add_char(ini_t* ini, section_t* s, uint32_t c)
{
    uint32_t lo = 0, hi = s->count;
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2U;
        if (s->chars[mid] < c)
            lo = mid + 1U;
        else
            hi = mid;
    }
    if (lo < s->count && s->chars[lo] == c)
        return;
    if (s->count == s->capacity)
    {
        uint32_t capacity = (s->capacity == 0) ? 64U : s->capacity * 2U;
        uint32_t* chars = Dmod_Malloc(capacity * sizeof(uint32_t));
        if (chars == NULL)
        {
            ini->failed = true;
            return;
        }
        if (s->count != 0)
            memcpy(chars, s->chars, s->count * sizeof(uint32_t));
        Dmod_Free(s->chars);
        s->chars = chars;
        s->capacity = capacity;
    }
    memmove(s->chars + lo + 1, s->chars + lo, (s->count - lo) * sizeof(uint32_t));
    s->chars[lo] = c;
    s->count++;
}

static section_t* section(ini_t* ini, const char* name, size_t length)
{
    for (uint32_t i = 0; i < ini->count; i++)
    {
        if (strlen(ini->sections[i].name) == length && strncmp(ini->sections[i].name, name, length) == 0)
            return &ini->sections[i];
    }
    if (ini->count >= MAX_SECTIONS || length + 1U > MAX_NAME)
    {
        ini->failed = true;
        return NULL;
    }
    section_t* s = &ini->sections[ini->count++];
    memset(s, 0, sizeof(*s));
    memcpy(s->name, name, length);
    s->name[length] = '\0';
    return s;
}

static void ini_free(ini_t* ini)
{
    for (uint32_t i = 0; i < ini->count; i++)
        Dmod_Free(ini->sections[i].chars);
    ini->count = 0;
}

static uint32_t parse_number(const char** p)
{
    const char* s = *p;
    uint32_t v = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        for (s += 2; ; s++)
        {
            char c = *s;
            uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                       : (c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10) : 16U;
            if (d > 15U)
                break;
            v = v * 16U + d;
        }
    }
    else
    {
        for (; *s >= '0' && *s <= '9'; s++)
            v = v * 10U + (uint32_t)(*s - '0');
    }
    *p = s;
    return v;
}

/* "-0.55" -> -55 */
static int32_t parse_tracking(const char* s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    bool minus = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    int32_t whole = (int32_t)parse_number(&s), fraction = 0, scale = 10;
    if (*s == '.')
    {
        for (s++; *s >= '0' && *s <= '9' && scale > 0; s++, scale /= 10)
            fraction += (*s - '0') * scale;
    }
    int32_t v = whole * 100 + fraction;
    return minus ? -v : v;
}

/* The .ini as written by write_ini() - or by hand: sections, size, chars, tracking */
static void parse_ini(ini_t* ini, const text_t* t)
{
    section_t* s = NULL;
    const char* p = t->data;
    while (p != NULL && *p != '\0')
    {
        const char* end = strchr(p, '\n');
        const char* line_end = (end != NULL) ? end : p + strlen(p);
        while (p < line_end && (*p == ' ' || *p == '\t'))
            p++;
        if (*p == '[')
        {
            const char* close = p;
            while (close < line_end && *close != ']')
                close++;
            if (close < line_end)
                s = section(ini, p + 1, (size_t)(close - p - 1));
        }
        else if (s != NULL && strncmp(p, "size", 4) == 0)
        {
            const char* v = strchr(p, '=');
            if (v != NULL && v < line_end)
            {
                v++;
                s->size = parse_number(&v);
            }
        }
        else if (s != NULL && strncmp(p, "tracking", 8) == 0)
        {
            const char* v = strchr(p, '=');
            if (v != NULL && v < line_end)
                s->tracking = parse_tracking(v + 1);
        }
        else if (s != NULL && strncmp(p, "chars", 5) == 0)
        {
            const char* v = strchr(p, '=');
            while (v != NULL && v < line_end)
            {
                v++;
                uint32_t from = parse_number(&v), to = from;
                if (*v == '-')
                {
                    v++;
                    to = parse_number(&v);
                }
                for (uint32_t c = from; c <= to && c - from < 0x10000u; c++)
                    add_char(ini, s, c);
                while (*v == ' ' || *v == '\t')
                    v++;
                if (*v != ',')
                    break;
            }
        }
        p = (end != NULL) ? end + 1 : NULL;
    }
}

static void write_ini(const ini_t* ini, text_t* t, const char* font, const char* view)
{
    text_fmt(t, "; %s - the sizes of the views made by todmvs (%s, ...): each with the characters\n", font, view);
    text_str(t, "; its text uses. todmvs adds to it; dmod makes a .dmvf of every section.\n");
    for (uint32_t i = 0; i < ini->count; i++)
    {
        const section_t* s = &ini->sections[i];
        text_fmt(t, "\n[%s]\nsize     = %u\n", s->name, (unsigned)s->size);
        if (s->tracking != 0)
        {
            int32_t v = (s->tracking < 0) ? -s->tracking : s->tracking;
            text_fmt(t, "tracking = %s%d.%d%d\n", (s->tracking < 0) ? "-" : "", (int)(v / 100), (int)((v / 10) % 10), (int)(v % 10));
        }
        text_str(t, "chars    = ");
        for (uint32_t k = 0; k < s->count; )
        {
            uint32_t first = s->chars[k], last = first;
            while (k + 1U < s->count && s->chars[k + 1U] == last + 1U)
                last = s->chars[++k];
            k++;
            if (first == last)
                text_fmt(t, "0x%X", (unsigned)first);
            else
                text_fmt(t, "0x%X-0x%X", (unsigned)first, (unsigned)last);
            if (k < s->count)
                text_str(t, ",");
        }
        if (s->count == 0)
            text_str(t, "0x20");
        text_str(t, "\n");
    }
}

int use_chars(font_use_t* use, const char* text, size_t length)
{
    const char* end = text + length;
    use->used = true;
    while (text < end)
    {
        uint32_t c = dmvsi_utf8_next(&text, end);
        uint32_t lo = 0, hi = use->count;
        while (lo < hi)
        {
            uint32_t mid = (lo + hi) / 2U;
            if (use->chars[mid] < c)
                lo = mid + 1U;
            else
                hi = mid;
        }
        if (lo < use->count && use->chars[lo] == c)
            continue;
        if (use->count == use->capacity)
        {
            uint32_t capacity = (use->capacity == 0) ? 32U : use->capacity * 2U;
            uint32_t* chars = Dmod_Malloc(capacity * sizeof(uint32_t));
            if (chars == NULL)
                return -ENOMEM;
            if (use->count != 0)
                memcpy(chars, use->chars, use->count * sizeof(uint32_t));
            Dmod_Free(use->chars);
            use->chars = chars;
            use->capacity = capacity;
        }
        memmove(use->chars + lo + 1, use->chars + lo, (use->count - lo) * sizeof(uint32_t));
        use->chars[lo] = c;
        use->count++;
    }
    return 0;
}

void uses_free(font_use_t* uses, uint32_t count)
{
    for (uint32_t i = 0; uses != NULL && i < count; i++)
        Dmod_Free(uses[i].chars);
    Dmod_Free(uses);
}

/* The fonts of `file` the view draws with: its copy in `dir`, and the sections of its .ini */
static int write_font(dmvsi_doc_t doc, const char* file, const char* dir, const char* view, const font_use_t* uses)
{
    char path[512];
    text_t t = { 0 };
    int ret = 0;

    Dmod_SnPrintf(path, sizeof(path), "%s/%s", dir, base_name(file));
    if ((ret = copy_file(file, path)) != 0)
        return ret;

    ini_t* ini = Dmod_Malloc(sizeof(*ini));         /* Large: not on the stack */
    if (ini == NULL)
        return -ENOMEM;
    memset(ini, 0, sizeof(*ini));
    Dmod_SnPrintf(path, sizeof(path), "%s/%s.ini", dir, base_name(file));
    if (text_load(&t, path))
        parse_ini(ini, &t);
    text_free(&t);

    dmvsi_font_t font;
    for (uint32_t i = 0; (font = dmvsi_font_at(doc, i)) != NULL; i++)
    {
        dmvsi_font_info_t info;
        char spec[MAX_NAME];
        uint32_t c;
        if (!uses[i].used || dmvsi_font_info(font, &info) != 0 || info.file == NULL || strcmp(info.file, file) != 0)
            continue;
        font_spec(font, spec, sizeof(spec));
        section_t* s = section(ini, spec, strlen(spec));
        if (s == NULL)
            break;
        s->size = info.size;
        s->tracking = info.tracking;
        for (uint32_t k = 0; k < uses[i].count; k++)
        {
            c = uses[i].chars[k];
            add_char(ini, s, c);
        }
    }

    if (!ini->failed)
        write_ini(ini, &t, base_name(file), view);
    ret = (ini->failed || t.failed) ? -ENOMEM : text_save(&t, path) ? 0 : -EIO;
    text_free(&t);
    ini_free(ini);
    Dmod_Free(ini);
    return ret;
}

int write_fonts(dmvsi_doc_t doc, const char* dir, const char* view, const font_use_t* uses)
{
    dmvsi_font_t font;
    for (uint32_t i = 0; (font = dmvsi_font_at(doc, i)) != NULL; i++)
    {
        dmvsi_font_info_t info;
        if (!uses[i].used || dmvsi_font_info(font, &info) != 0 || info.file == NULL)
            continue;

        /* Once per file: at its first font the view draws with */
        bool first = true;
        for (uint32_t k = 0; k < i && first; k++)
        {
            dmvsi_font_info_t other;
            if (uses[k].used && dmvsi_font_info(dmvsi_font_at(doc, k), &other) == 0 && other.file != NULL &&
                strcmp(other.file, info.file) == 0)
                first = false;
        }
        if (!first)
            continue;
        int ret = write_font(doc, info.file, dir, view, uses);
        if (ret != 0)
            return ret;
    }
    return 0;
}

/* ---- Images ---- */

#define MAX_IMAGE_SECTIONS  32u

typedef struct
{
    char        name[MAX_NAME];
    uint32_t    width, height;      /* 0: its own size */
    uint32_t    blur;
} image_section_t;

/* The sections of an image's .ini (its sizes and blurs) */
static uint32_t parse_image_ini(const text_t* t, image_section_t* out, uint32_t max)
{
    uint32_t count = 0;
    image_section_t* s = NULL;
    const char* p = t->data;
    while (p != NULL && *p != '\0')
    {
        const char* end = strchr(p, '\n');
        const char* line_end = (end != NULL) ? end : p + strlen(p);
        while (p < line_end && (*p == ' ' || *p == '\t'))
            p++;
        const char* v = strchr(p, '=');
        if (v != NULL && v >= line_end)
            v = NULL;
        if (*p == '[')
        {
            const char* close = p;
            while (close < line_end && *close != ']')
                close++;
            size_t n = (size_t)(close - p - 1);
            s = NULL;
            if (close < line_end && count < max && n < MAX_NAME)
            {
                s = &out[count++];
                memset(s, 0, sizeof(*s));
                memcpy(s->name, p + 1, n);
            }
        }
        else if (s != NULL && v != NULL && strncmp(p, "size", 4) == 0)
        {
            v++;
            s->width = parse_number(&v);
            if (*v == 'x')
            {
                v++;
                s->height = parse_number(&v);
            }
        }
        else if (s != NULL && v != NULL && strncmp(p, "blur", 4) == 0)
        {
            v++;
            s->blur = parse_number(&v);
        }
        p = (end != NULL) ? end + 1 : NULL;
    }
    return count;
}

int image_section(const char* dir, const char* view, const char* image, const char* name,
                  uint32_t width, uint32_t height, uint32_t blur)
{
    char path[512];
    text_t t = { 0 };
    image_section_t* sections = Dmod_Malloc(MAX_IMAGE_SECTIONS * sizeof(image_section_t));
    if (sections == NULL)
        return -ENOMEM;
    Dmod_SnPrintf(path, sizeof(path), "%s/%s.ini", dir, base_name(image));
    uint32_t count = text_load(&t, path) ? parse_image_ini(&t, sections, MAX_IMAGE_SECTIONS) : 0;
    text_free(&t);

    bool found = false;
    for (uint32_t i = 0; i < count && !found; i++)
        found = strcmp(sections[i].name, name) == 0;
    if (!found && count < MAX_IMAGE_SECTIONS && strlen(name) < MAX_NAME)
    {
        image_section_t* s = &sections[count++];
        memset(s, 0, sizeof(*s));
        strcpy(s->name, name);
        s->width = width;
        s->height = height;
        s->blur = blur;
    }

    text_fmt(&t, "; %s - the sizes of the views made by todmvs (%s, ...) draw it at. todmvs adds\n", base_name(image), view);
    text_str(&t, "; to it; dmod makes a .dmvi of every section.\n");
    for (uint32_t i = 0; i < count; i++)
    {
        const image_section_t* s = &sections[i];
        text_fmt(&t, "\n[%s]\n", s->name);
        if (s->width != 0 && s->height != 0)
            text_fmt(&t, "size = %ux%u\n", (unsigned)s->width, (unsigned)s->height);
        if (s->blur != 0)
            text_fmt(&t, "blur = %u\n", (unsigned)s->blur);
    }
    int ret = t.failed ? -ENOMEM : text_save(&t, path) ? 0 : -EIO;
    text_free(&t);
    Dmod_Free(sections);
    return ret;
}
