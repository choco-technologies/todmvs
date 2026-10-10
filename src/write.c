#define DMOD_ENABLE_REGISTRATION ON
#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Writing a view: the document's nodes in their order, at whole pixels
 * (every edge rounded on its own, as a browser snaps a box), relative to
 * the box they are in.
 *
 *  - A group is a BOX only when it clips, fades or scrolls; its OPAQUE flag
 *    when its first shape covers it with opaque pixels. A group that does
 *    not clip is a box as large as what is in it.
 *  - A shape outside the screen or its box is left out, and one that paints
 *    nothing (transparent); a group with opacity 0 too.
 *  - Text is TEXT on its baseline - its rectangle from the font's ascent,
 *    a little wider than the text, so the last glyph's ink is not clipped.
 *
 * The code is built first; the fonts and gradients it uses are declared
 * before it, once each.
 */

#define MAX_NAME        48u
#define MAX_BOX_NAMES   1024u

#define MAX_EASINGS     8u
#define MAX_CALLS       8u              /* dmview's CALL stack */
#define MAX_MERGES      64u

/* A translucent fill painted right over an opaque one of its rectangle: one opaque fill of both */
typedef struct
{
    const dmvsi_node_t* under;          /* Not written */
    const dmvsi_node_t* over;           /* Written with `paint` */
    dmvsi_paint_t       paint;
    dmvsi_unit_t        radius;
} merge_t;
#define EASE_STEPS      16u

/* What the writer knows of a variable */
typedef struct
{
    uint8_t     bound;                  /* DMVSI_BIND_* + 1 of the group it is bound to, 0: none */
    int32_t     origin;                 /* X / Y: the pixel its parent box starts at (values are relative to it) */
    int32_t     shift;                  /* X / Y: where the box starts, from its group's rectangle (what it holds) */
    bool        animated;
} var_info_t;

/* A cubic-bezier easing, as the points of its piecewise linear approximation */
typedef struct
{
    int16_t     curve[4];
    int32_t     y[EASE_STEPS + 1];      /* 0 ... 1000, at x = i / EASE_STEPS */
} easing_t;

typedef struct
{
    dmvsi_doc_t             doc;
    text_t                  code;
    dmvsi_paint_t*          gradients;
    uint32_t                gradient_count;
    uint32_t                gradient_capacity;
    char                  (*fonts)[MAX_NAME];  /* The .font name of the document's font i */
    font_use_t*             uses;               /* ... what the view draws with it */
    uint32_t                font_count;
    char                  (*boxes)[MAX_NAME];  /* Box names used */
    uint32_t                box_count;
    const char*             dir;
    bool                    no_assets;
    int                     error;
    libtodmvs_result_t      r;
    var_info_t*             vars;               /* Of the document's variables, by index */
    uint32_t                var_count;
    easing_t                easings[MAX_EASINGS];
    uint32_t                easing_count;
    bool                    animates;
    merge_t                 merges[MAX_MERGES];
    uint32_t                merge_count;
    uint32_t                show_labels;
} writer_t;

/* ---- Names ---- */

/* `name` as an identifier: letters, digits, '_', not starting with a digit */
static void identifier(const char* name, const char* prefix, char* out, size_t size)
{
    size_t n = 0;
    if (name[0] >= '0' && name[0] <= '9')
    {
        size_t p = strlen(prefix);
        memcpy(out, prefix, p);
        n = p;
    }
    for (const char* s = name; *s != '\0' && n + 1U < size; s++)
    {
        char c = *s;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out[n++] = ok ? c : '_';
    }
    out[n] = '\0';
    if (n == 0)
        Dmod_SnPrintf(out, size, "%s", prefix);
}

static bool name_used(char (*names)[MAX_NAME], uint32_t count, const char* name)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (strcmp(names[i], name) == 0)
            return true;
    }
    return false;
}

/* A box name not used yet: the group's name (an id), else box<n> */
static const char* box_name(writer_t* w, const char* name)
{
    static char none[] = "box";
    char base[MAX_NAME], unique[MAX_NAME];
    if (w->box_count >= MAX_BOX_NAMES)
    {
        w->error = -ENOMEM;
        return none;
    }
    identifier((name != NULL) ? name : "box", "box_", base, sizeof(base) - 8U);
    if (name != NULL && !name_used(w->boxes, w->box_count, base))
        strcpy(unique, base);
    else
    {
        for (uint32_t n = w->box_count + 1U; ; n++)
        {
            Dmod_SnPrintf(unique, sizeof(unique), "%s%u", base, (unsigned)n);
            if (!name_used(w->boxes, w->box_count, unique))
                break;
        }
    }
    strcpy(w->boxes[w->box_count], unique);
    return w->boxes[w->box_count++];
}

static int name_fonts(writer_t* w)
{
    while (dmvsi_font_at(w->doc, w->font_count) != NULL)
        w->font_count++;
    if (w->font_count == 0)
        return 0;
    if ((w->fonts = Dmod_Malloc(w->font_count * MAX_NAME)) == NULL ||
        (w->uses = Dmod_Malloc(w->font_count * sizeof(font_use_t))) == NULL)
        return -ENOMEM;
    memset(w->uses, 0, w->font_count * sizeof(font_use_t));
    for (uint32_t i = 0; i < w->font_count; i++)
    {
        char spec[MAX_NAME], name[MAX_NAME];
        font_spec(dmvsi_font_at(w->doc, i), spec, sizeof(spec));
        identifier(spec, "f_", name, sizeof(name) - 8U);
        if (name_used(w->fonts, i, name))
        {
            size_t n = strlen(name);
            Dmod_SnPrintf(name + n, sizeof(name) - n, "_%u", (unsigned)i);
        }
        strcpy(w->fonts[i], name);
    }
    return 0;
}

/* The .font name of a font the view draws text with: from now on declared, the text's characters in it */
static const char* font_name(writer_t* w, dmvsi_font_t font, const char* text, size_t length)
{
    for (uint32_t i = 0; i < w->font_count; i++)
    {
        if (dmvsi_font_at(w->doc, i) == font)
        {
            if (use_chars(&w->uses[i], text, length) != 0)
                w->error = -ENOMEM;
            return w->fonts[i];
        }
    }
    return "?";
}

/* ---- Paints ---- */

static bool paint_visible(const dmvsi_paint_t* p)
{
    if (p->kind == DMVSI_PAINT_COLOR)
        return (p->color >> 24) != 0;
    for (uint32_t i = 0; i < p->count; i++)
    {
        if ((p->stops[i].color >> 24) != 0)
            return true;
    }
    return false;
}

static bool paint_opaque(const dmvsi_paint_t* p)
{
    if (p->kind == DMVSI_PAINT_COLOR)
        return (p->color >> 24) == 0xFFu;
    for (uint32_t i = 0; i < p->count; i++)
    {
        if ((p->stops[i].color >> 24) != 0xFFu)
            return false;
    }
    return true;
}

/* The gradient's index, declared when it is new - the same gradient once */
static uint32_t gradient(writer_t* w, const dmvsi_paint_t* p)
{
    dmvsi_paint_t g;
    memset(&g, 0, sizeof(g));
    g.kind = p->kind;
    g.count = p->count;
    memcpy(g.stops, p->stops, p->count * sizeof(dmvsi_stop_t));
    if (p->kind == DMVSI_PAINT_LINEAR)
        g.angle = (int16_t)(((p->angle % 360) + 360) % 360);
    else
    {
        g.cx = p->cx;
        g.cy = p->cy;
        g.rx = p->rx;
        g.ry = p->ry;
    }
    for (uint32_t i = 0; i < w->gradient_count; i++)
    {
        if (memcmp(&w->gradients[i], &g, sizeof(g)) == 0)
            return i;
    }
    if (w->gradient_count == w->gradient_capacity)
    {
        uint32_t capacity = (w->gradient_capacity == 0) ? 16U : w->gradient_capacity * 2U;
        dmvsi_paint_t* gradients = Dmod_Malloc(capacity * sizeof(dmvsi_paint_t));
        if (gradients == NULL)
        {
            w->error = -ENOMEM;
            return 0;
        }
        if (w->gradient_count != 0)
            memcpy(gradients, w->gradients, w->gradient_count * sizeof(dmvsi_paint_t));
        Dmod_Free(w->gradients);
        w->gradients = gradients;
        w->gradient_capacity = capacity;
    }
    w->gradients[w->gradient_count] = g;
    return w->gradient_count++;
}

static void paint(writer_t* w, const dmvsi_paint_t* p)
{
    if (p->kind == DMVSI_PAINT_COLOR)
        text_color(&w->code, p->color);
    else
        text_fmt(&w->code, "g%u", (unsigned)(gradient(w, p) + 1U));
}

/* 1/100 % -> "33.3" (the 1/1000 the assembler keeps) */
static void position(text_t* t, uint32_t v)
{
    uint32_t m = (v + 5U) / 10U;
    if (m % 10U == 0)
        text_fmt(t, "%u", (unsigned)(m / 10U));
    else
        text_fmt(t, "%u.%u", (unsigned)(m / 10U), (unsigned)(m % 10U));
}

/* 1/100 % -> whole % (rounded) */
static int32_t percent(int32_t v)
{
    return (v >= 0) ? (v + 50) / 100 : -((-v + 50) / 100);
}

/* The assembler reads lines of up to this many characters */
#define MAX_LINE        254u

/* One declaration into t */
static void declare_gradient(text_t* t, uint32_t i, const dmvsi_paint_t* g)
{
    text_fmt(t, ".gradient g%u, ", (unsigned)(i + 1U));
    if (g->kind == DMVSI_PAINT_LINEAR)
        text_fmt(t, "LINEAR, %d", (int)g->angle);
    else
    {
        int32_t rx = percent(g->rx), ry = percent(g->ry);
        text_fmt(t, "RADIAL, %d, %d, %d, %d", (int)percent(g->cx), (int)percent(g->cy), (int)((rx < 1) ? 1 : rx),
                 (int)((ry < 1) ? 1 : ry));
    }
    for (uint32_t k = 0; k < g->count; k++)
    {
        text_str(t, ", ");
        text_color(t, g->stops[k].color);
        text_str(t, " ");
        position(t, g->stops[k].position);
    }
    text_str(t, "\n");
}

static void declare_gradients(writer_t* w, text_t* t)
{
    if (w->gradient_count != 0)
        text_str(t, "\n");
    for (uint32_t i = 0; i < w->gradient_count; i++)
    {
        /* Too long for a line: every second stop between the ends left out, until it fits */
        dmvsi_paint_t g = w->gradients[i];
        for (;;)
        {
            text_t line = { 0 };
            declare_gradient(&line, i, &g);
            bool fits = line.failed || line.size <= MAX_LINE + 1U || g.count <= 2U;
            text_free(&line);
            if (fits)
                break;
            uint8_t n = 1;
            for (uint8_t k = 2; k + 1U < g.count; k += 2)
                g.stops[n++] = g.stops[k];
            g.stops[n++] = g.stops[g.count - 1U];
            g.count = n;
        }
        declare_gradient(t, i, &g);
    }
}

static const char* var_name(const writer_t* w, dmvsi_var_t var);

/* ---- Instructions ---- */

/* "        RECT    " - the mnemonic in its column */
static void op(writer_t* w, const char* mnemonic)
{
    static const char spaces[] = "        ";
    size_t n = strlen(mnemonic);
    text_add(&w->code, spaces, 8);
    text_add(&w->code, mnemonic, n);
    text_add(&w->code, spaces, (n < 8U) ? 8U - n : 1U);
}

static void numbers(writer_t* w, const int32_t* v, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        text_fmt(&w->code, (i == 0) ? "%d" : ", %d", (int)v[i]);
}

/* Text's rectangle: from the top of its line, a little wider than the text */
static box_t text_box(const dmvsi_text_t* t)
{
    dmvsi_font_info_t info;
    (void)dmvsi_font_info(t->font, &info);
    int32_t x = snap(t->x), top = snap(t->baseline) - info.ascent;
    int32_t width = dmvsi_text_width(t->font, t->text, t->length);
    box_t b = { x, top, x + width + (info.ascent + info.descent) / 2, top + info.ascent + info.descent };
    return b;
}

/* What a node paints, in whole pixels */
static box_t extent(const dmvsi_node_t* n)
{
    if (n->kind == DMVSI_NODE_TEXT)
        return text_box(&n->u.text);
    if (n->kind != DMVSI_NODE_GROUP || (n->u.group.flags & DMVSI_GROUP_CLIP) != 0)
        return snap_rect(&n->bounds);
    box_t all = snap_rect(&n->u.group.rect);       /* At least the rectangle it was given (a button's) */
    if (box_empty(&all))
        all.x0 = all.y0 = all.x1 = all.y1 = 0;
    for (const dmvsi_node_t* c = n->first; c != NULL; c = c->next)
    {
        box_t e = extent(c);
        if (box_empty(&e))
            continue;
        if (box_empty(&all))
            all = e;
        else
        {
            all.x0 = (e.x0 < all.x0) ? e.x0 : all.x0;
            all.y0 = (e.y0 < all.y0) ? e.y0 : all.y0;
            all.x1 = (e.x1 > all.x1) ? e.x1 : all.x1;
            all.y1 = (e.y1 > all.y1) ? e.y1 : all.y1;
        }
    }
    return all;
}

static bool box_same(const box_t* a, const box_t* b)
{
    return a->x0 == b->x0 && a->y0 == b->y0 && a->x1 == b->x1 && a->y1 == b->y1;
}

static bool box_covers(const box_t* outer, const box_t* inner)
{
    return outer->x0 <= inner->x0 && outer->y0 <= inner->y0 && outer->x1 >= inner->x1 && outer->y1 >= inner->y1;
}

static void write_fill(writer_t* w, const dmvsi_fill_t* f, const box_t* origin)
{
    box_t b = snap_rect(&f->rect);
    int32_t r = snap(f->radius);
    if (r == 0 && box_covers(&b, origin))
    {
        op(w, "FILL");
    }
    else
    {
        int32_t v[5] = { b.x0 - origin->x0, b.y0 - origin->y0, b.x1 - b.x0, b.y1 - b.y0, r };
        op(w, (r == 0) ? "RECT" : "RRECT");
        numbers(w, v, (r == 0) ? 4U : 5U);
        text_str(&w->code, ", ");
    }
    paint(w, &f->paint);
    text_str(&w->code, "\n");
}

static void write_frame(writer_t* w, const dmvsi_frame_t* f, const box_t* origin)
{
    box_t b = snap_rect(&f->rect);
    int32_t r = snap(f->radius), t = snap(f->width);
    int32_t v[6] = { b.x0 - origin->x0, b.y0 - origin->y0, b.x1 - b.x0, b.y1 - b.y0, r, (t < 1) ? 1 : t };
    if (r == 0)
    {
        v[4] = v[5];
        op(w, "FRAME");
        numbers(w, v, 5);
    }
    else
    {
        op(w, "RFRAME");
        numbers(w, v, 6);
    }
    text_str(&w->code, ", ");
    paint(w, &f->paint);
    text_str(&w->code, "\n");
}

static void write_shadow(writer_t* w, const dmvsi_shadow_t* s, const box_t* origin, const box_t* clip)
{
    piece_t pieces[MAX_PIECES];
    uint32_t n = shadow_pieces(s, pieces);
    for (uint32_t i = 0; i < n; i++)
    {
        const box_t* b = &pieces[i].box;
        box_t seen = box_and(*b, *clip);
        if (box_empty(&seen))
            continue;
        if (pieces[i].kind == PIECE_CIRCLE)
        {
            int32_t v[3] = { (b->x0 + b->x1) / 2 - origin->x0, (b->y0 + b->y1) / 2 - origin->y0, (b->x1 - b->x0) / 2 };
            op(w, "CIRCLE");
            numbers(w, v, 3);
        }
        else
        {
            int32_t v[4] = { b->x0 - origin->x0, b->y0 - origin->y0, b->x1 - b->x0, b->y1 - b->y0 };
            op(w, "RECT");
            numbers(w, v, 4);
        }
        text_str(&w->code, ", ");
        paint(w, &pieces[i].paint);
        text_str(&w->code, "\n");
        w->r.instructions++;
    }
}

/* Every character of a font into its use: what a text variable may show is there (dmvsi added it) */
static void use_all_chars(writer_t* w, dmvsi_font_t font)
{
    char utf8[4];
    uint32_t cp = 0;
    for (uint32_t i = 0; dmvsi_font_char(font, i, &cp); i++)
    {
        size_t n = (cp < 0x80u) ? 1u : (cp < 0x800u) ? 2u : (cp < 0x10000u) ? 3u : 4u;
        if (n == 1u)
            utf8[0] = (char)cp;
        else if (n == 2u)
        {
            utf8[0] = (char)(0xC0u | (cp >> 6));
            utf8[1] = (char)(0x80u | (cp & 0x3Fu));
        }
        else if (n == 3u)
        {
            utf8[0] = (char)(0xE0u | (cp >> 12));
            utf8[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            utf8[2] = (char)(0x80u | (cp & 0x3Fu));
        }
        else
        {
            utf8[0] = (char)(0xF0u | (cp >> 18));
            utf8[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            utf8[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            utf8[3] = (char)(0x80u | (cp & 0x3Fu));
        }
        (void)font_name(w, font, utf8, n);
    }
}

static void write_text(writer_t* w, const dmvsi_text_t* t, const box_t* origin)
{
    box_t b = text_box(t);
    if (t->var != 0 && t->width > 0)
        b.x1 = b.x0 + snap(t->width);                   /* The room it is put in as it changes */
    int32_t v[4] = { b.x0 - origin->x0, b.y0 - origin->y0, b.x1 - b.x0, b.y1 - b.y0 };
    op(w, "TEXT");
    numbers(w, v, 4);
    text_str(&w->code, ", ");
    if (t->var != 0)
    {
        use_all_chars(w, t->font);
        text_fmt(&w->code, "$%s", var_name(w, t->var));
    }
    else
        text_string(&w->code, t->text, t->length);
    text_fmt(&w->code, ", %s, ", font_name(w, t->font, t->text, t->length));
    paint(w, &t->paint);
    text_str(&w->code, (t->var == 0 || t->align == DMVSI_TEXT_LEFT) ? ", LEFT|TOP\n" :
                       (t->align == DMVSI_TEXT_CENTER) ? ", CENTER|TOP\n" : ", RIGHT|TOP\n");
}

/*
 * The image next to the view, and the section of its .ini dmod makes the
 * .dmvi of: <name> at its own size, <name>-<w>x<h>[-b<blur>] scaled (and
 * blurred). dmview draws it unscaled, placed in the rectangle and clipped.
 */
static void write_image(writer_t* w, const dmvsi_image_t* im, const box_t* origin)
{
    char stem_name[MAX_NAME], name[MAX_NAME + 32], copy[512];
    const char* base = base_name(im->path);
    const char* dot = strrchr(base, '.');
    size_t stem = (dot != NULL && dot != base) ? (size_t)(dot - base) : strlen(base);
    if (stem >= sizeof(stem_name))
        stem = sizeof(stem_name) - 1U;
    memcpy(stem_name, base, stem);
    stem_name[stem] = '\0';
    int32_t iw = snap(im->width), ih = snap(im->height), blur = snap(im->blur);
    if (iw > 0 && ih > 0 && blur > 0)
        Dmod_SnPrintf(name, sizeof(name), "%s-%dx%d-b%d", stem_name, (int)iw, (int)ih, (int)blur);
    else if (iw > 0 && ih > 0)
        Dmod_SnPrintf(name, sizeof(name), "%s-%dx%d", stem_name, (int)iw, (int)ih);
    else if (blur > 0)
        Dmod_SnPrintf(name, sizeof(name), "%s-b%d", stem_name, (int)blur);
    else
        Dmod_SnPrintf(name, sizeof(name), "%s", stem_name);
    if (!w->no_assets && w->error == 0)
    {
        Dmod_SnPrintf(copy, sizeof(copy), "%s/%s", w->dir, base);
        int ret = copy_file(im->path, copy);
        if (ret == 0)
            ret = image_section(w->dir, dmvsi_view_name(w->doc), im->path, name, (uint32_t)((iw > 0) ? iw : 0),
                                (uint32_t)((ih > 0) ? ih : 0), (uint32_t)((blur > 0) ? blur : 0));
        if (ret != 0)
        {
            DMOD_LOG_ERROR("todmvs: cannot copy the image %s\n", im->path);
            w->error = ret;
        }
    }

    box_t b = snap_rect(&im->rect);
    int32_t v[4] = { b.x0 - origin->x0, b.y0 - origin->y0, b.x1 - b.x0, b.y1 - b.y0 };
    bool mask = (im->flags & DMVSI_IMAGE_MASK) != 0;
    op(w, mask ? "ICON" : "IMAGE");
    numbers(w, v, 4);
    text_str(&w->code, ", ");
    strcat(name, ".dmvi");
    text_string(&w->code, name, strlen(name));
    if (mask)
    {
        text_str(&w->code, ", ");
        paint(w, &im->paint);
    }
    text_str(&w->code, (im->flags & DMVSI_IMAGE_CENTER) ? ", CENTER" : (im->flags & DMVSI_IMAGE_RIGHT) ? ", RIGHT" : ", LEFT");
    text_str(&w->code, (im->flags & DMVSI_IMAGE_MIDDLE) ? "|MIDDLE\n" : (im->flags & DMVSI_IMAGE_BOTTOM) ? "|BOTTOM\n" : "|TOP\n");
}

/* Its first shape fills it with opaque pixels */
static bool group_opaque(const dmvsi_node_t* n, const box_t* rect)
{
    const dmvsi_node_t* first = n->first;
    if (n->u.group.opacity != 255 || first == NULL || first->kind != DMVSI_NODE_RECT || snap(first->u.fill.radius) != 0)
        return false;
    box_t b = snap_rect(&first->u.fill.rect);
    return box_covers(&b, rect) && paint_opaque(&first->u.fill.paint);
}


/* ---- Fills over fills ---- */

/* c over an opaque bg */
static uint32_t over(uint32_t c, uint32_t bg)
{
    uint32_t a = c >> 24, out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8)
    {
        uint32_t x = (c >> s) & 0xFFu, y = (bg >> s) & 0xFFu;
        out |= (((x * a + y * (255U - a)) + 127U) / 255U) << s;
    }
    return out;
}

static bool translucent(const dmvsi_paint_t* p)
{
    if (p->kind == DMVSI_PAINT_COLOR)
        return (p->color >> 24) != 0xFFu;
    for (uint32_t i = 0; i < p->count; i++)
    {
        if ((p->stops[i].color >> 24) != 0xFFu)
            return true;
    }
    return false;
}

/* The leaves in painting order: a translucent fill right after an opaque color of the same
 * rectangle - nothing between them, in no group of its own that fades or moves - is merged:
 * the screen gets the blend as one opaque fill (an opaque gradient is dithered on RGB565, a
 * translucent one is not), and one fill less */
static void find_merges(writer_t* w, const dmvsi_node_t* n, const dmvsi_node_t** previous)
{
    for (; n != NULL; n = n->next)
    {
        if (n->kind == DMVSI_NODE_GROUP)
        {
            bool own = n->u.group.opacity != 255 || n->bind[DMVSI_BIND_X] != 0 || n->bind[DMVSI_BIND_Y] != 0 ||
                       n->bind[DMVSI_BIND_OPACITY] != 0 || n->show_var[0] != 0;
            if (own)
                *previous = NULL;
            find_merges(w, n->first, previous);
            if (own)
                *previous = NULL;
            continue;
        }
        const dmvsi_node_t* under = *previous;
        *previous = n;
        if (n->kind != DMVSI_NODE_RECT || under == NULL || under->kind != DMVSI_NODE_RECT || w->merge_count >= MAX_MERGES)
            continue;
        const dmvsi_fill_t* a = &under->u.fill;
        const dmvsi_fill_t* b = &n->u.fill;
        box_t ra = snap_rect(&a->rect), rb = snap_rect(&b->rect);
        if (a->paint.kind != DMVSI_PAINT_COLOR || (a->paint.color >> 24) != 0xFFu || !translucent(&b->paint) ||
            !box_same(&ra, &rb) || snap(b->radius) > snap(a->radius))
            continue;
        merge_t* m = &w->merges[w->merge_count++];
        m->under = under;
        m->over = n;
        m->paint = b->paint;
        m->radius = a->radius;
        if (b->paint.kind == DMVSI_PAINT_COLOR)
            m->paint.color = over(b->paint.color, a->paint.color);
        for (uint32_t i = 0; i < b->paint.count; i++)
            m->paint.stops[i].color = over(b->paint.stops[i].color, a->paint.color);
        *previous = NULL;
    }
}

static const merge_t* merged(const writer_t* w, const dmvsi_node_t* n, bool* skip)
{
    for (uint32_t i = 0; i < w->merge_count; i++)
    {
        if (w->merges[i].under == n)
        {
            *skip = true;
            return NULL;
        }
        if (w->merges[i].over == n)
            return &w->merges[i];
    }
    return NULL;
}

static void write_nodes(writer_t* w, const dmvsi_node_t* first, const box_t* origin, const box_t* clip);
static const char* var_name(const writer_t* w, dmvsi_var_t var);

static void write_group(writer_t* w, const dmvsi_node_t* n, const box_t* origin, const box_t* clip)
{
    const dmvsi_group_t* g = &n->u.group;
    bool clips = (g->flags & DMVSI_GROUP_CLIP) != 0;
    bool scrolls = g->scroll_w > 0 && g->scroll_h > 0;
    bool bound = n->bind[DMVSI_BIND_X] != 0 || n->bind[DMVSI_BIND_Y] != 0 || n->bind[DMVSI_BIND_OPACITY] != 0 ||
                 n->bind[DMVSI_BIND_W] != 0 || n->bind[DMVSI_BIND_H] != 0;
    if (g->opacity == 0 && n->bind[DMVSI_BIND_OPACITY] == 0)
    {
        w->r.skipped++;
        return;
    }
    /* A clip as large as what clips already (the screen, its box) clips nothing: no box for it */
    box_t given = snap_rect(&g->rect);
    bool needless = clips && box_covers(&given, clip) && box_covers(&given, origin);
    if ((!clips || needless) && g->opacity == 255 && !scrolls && !bound && n->click == 0)
    {
        write_nodes(w, n->first, origin, clip);
        return;
    }

    box_t rect = clips ? snap_rect(&g->rect) : extent(n);
    int32_t v[4] = { rect.x0 - origin->x0, rect.y0 - origin->y0, rect.x1 - rect.x0, rect.y1 - rect.y0 };
    op(w, "BOX");
    text_fmt(&w->code, "@%s, ", box_name(w, g->name));
    for (uint32_t k = 0; k < 2U; k++)
    {
        /* Its position from a variable: relative to this box's parent, as BOX takes it */
        dmvsi_var_t var = n->bind[(k == 0) ? DMVSI_BIND_X : DMVSI_BIND_Y];
        if (var != 0 && var <= w->var_count)
        {
            const char* name = NULL;
            (void)dmvsi_var_at(w->doc, var - 1U, &name, NULL);
            box_t given = snap_rect(&g->rect);
            w->vars[var - 1U].bound = (uint8_t)(((k == 0) ? DMVSI_BIND_X : DMVSI_BIND_Y) + 1U);
            w->vars[var - 1U].origin = (k == 0) ? origin->x0 : origin->y0;
            w->vars[var - 1U].shift = (k == 0) ? rect.x0 - given.x0 : rect.y0 - given.y0;
            text_fmt(&w->code, "$%s, ", name);
        }
        else
            text_fmt(&w->code, "%d, ", (int)v[k]);
    }
    for (uint32_t k = 0; k < 2U; k++)
    {
        /* Its size from a variable: pixels, as a handler computes them */
        dmvsi_var_t var = n->bind[(k == 0) ? DMVSI_BIND_W : DMVSI_BIND_H];
        if (k > 0)
            text_str(&w->code, ", ");
        if (var != 0 && var <= w->var_count)
        {
            w->vars[var - 1U].bound = (uint8_t)(((k == 0) ? DMVSI_BIND_W : DMVSI_BIND_H) + 1U);
            text_fmt(&w->code, "$%s", var_name(w, var));
        }
        else
            text_fmt(&w->code, "%d", (int)v[2U + k]);
    }
    if (n->bind[DMVSI_BIND_OPACITY] == 0 && group_opaque(n, &rect))  /* Moving, it covers its box all the same */
        text_str(&w->code, ", OPAQUE");
    text_str(&w->code, "\n");
    w->r.boxes++;
    /* A box that moves: what is in it as it is in it, wherever it is */
    box_t inside = (n->bind[DMVSI_BIND_X] != 0 || n->bind[DMVSI_BIND_Y] != 0) ? rect : box_and(*clip, rect);
    if (scrolls)
    {
        int32_t s[2] = { snap(g->scroll_w), snap(g->scroll_h) };
        op(w, "SCROLL");
        numbers(w, s, 2);
        text_str(&w->code, "\n");
        box_t content = { rect.x0, rect.y0, rect.x0 + s[0], rect.y0 + s[1] };
        inside = content;               /* What can be scrolled into view */
    }
    dmvsi_var_t alpha = n->bind[DMVSI_BIND_OPACITY];
    if (alpha != 0 && alpha <= w->var_count)
    {
        const char* name = NULL;
        (void)dmvsi_var_at(w->doc, alpha - 1U, &name, NULL);
        w->vars[alpha - 1U].bound = DMVSI_BIND_OPACITY + 1U;
        op(w, "OPACITY");
        text_fmt(&w->code, "$%s\n", name);
    }
    else if (g->opacity != 255)
    {
        op(w, "OPACITY");
        text_fmt(&w->code, "%u\n", (unsigned)g->opacity);
    }
    if (n->click != 0)
    {
        op(w, "ON");
        text_fmt(&w->code, "CLICK, h%u\n", (unsigned)n->click);
    }
    write_nodes(w, n->first, &rect, &inside);
    text_str(&w->code, "        END\n");
}

static void write_nodes(writer_t* w, const dmvsi_node_t* first, const box_t* origin, const box_t* clip)
{
    for (const dmvsi_node_t* n = first; n != NULL && w->error == 0; n = n->next)
    {
        box_t e = extent(n);
        box_t seen = box_and(e, *clip);
        bool moves = n->kind == DMVSI_NODE_GROUP && (n->bind[DMVSI_BIND_X] != 0 || n->bind[DMVSI_BIND_Y] != 0);
        if (box_empty(&seen) && !moves)         /* A box that moves may come into view */
        {
            w->r.skipped++;
            continue;
        }
        switch (n->kind)
        {
            case DMVSI_NODE_GROUP:
            {
                /* Shown only on its conditions: jumped over else */
                uint32_t label = 0;
                for (uint32_t k = 0; k < DMVSI_MAX_SHOW && n->show_var[k] != 0; k++)
                {
                    if (label == 0)
                        label = ++w->show_labels;
                    const char* name = (n->show_var[k] == DMVSI_VAR_PRESSED) ? "box.pressed" : var_name(w, n->show_var[k]);
                    text_fmt(&w->code, "        JNE     $%s, %d, .g%u\n", name, (int)n->show_value[k], (unsigned)label);
                }
                write_group(w, n, origin, clip);
                if (label != 0)
                    text_fmt(&w->code, ".g%u:\n", (unsigned)label);
                continue;
            }
            case DMVSI_NODE_RECT:
            {
                bool skip = false;
                const merge_t* m = merged(w, n, &skip);
                if (skip)
                    continue;                   /* Painted with the fill over it */
                if (!paint_visible(&n->u.fill.paint))
                    break;
                dmvsi_fill_t f = n->u.fill;
                if (m != NULL)
                {
                    f.paint = m->paint;
                    f.radius = m->radius;
                }
                write_fill(w, &f, origin);
                w->r.instructions++;
                continue;
            }
            case DMVSI_NODE_FRAME:
                if (!paint_visible(&n->u.frame.paint))
                    break;
                write_frame(w, &n->u.frame, origin);
                w->r.instructions++;
                continue;
            case DMVSI_NODE_SHADOW:
                write_shadow(w, &n->u.shadow, origin, clip);
                continue;
            case DMVSI_NODE_TEXT:
                if (!paint_visible(&n->u.text.paint) || n->u.text.length == 0)
                    break;
                write_text(w, &n->u.text, origin);
                w->r.instructions++;
                continue;
            case DMVSI_NODE_IMAGE:
                write_image(w, &n->u.image, origin);
                w->r.instructions++;
                continue;
            default:
                break;
        }
        w->r.skipped++;
    }
}


/* ---- Behaviour: variables, handlers, animations ---- */

static const char* var_name(const writer_t* w, dmvsi_var_t var)
{
    const char* name = "?";
    (void)dmvsi_var_at(w->doc, var - 1U, &name, NULL);
    return name;
}

/* A value of a variable as the view holds it: a position relative to its box's parent, in pixels */
static int32_t view_value(const writer_t* w, dmvsi_var_t var, int32_t value)
{
    const var_info_t* v = &w->vars[var - 1U];
    if (v->bound == DMVSI_BIND_X + 1U || v->bound == DMVSI_BIND_Y + 1U)
        return snap(value) + v->shift - v->origin;
    if (v->bound == DMVSI_BIND_OPACITY + 1U)
        return (value < 0) ? 0 : (value > 255) ? 255 : value;
    return value;
}

/* The cubic-bezier of CSS at x (0 ... 1): its y, by the t that gives x */
static double bezier_at(const int16_t* c, double x)
{
    double x1 = c[0] / 1000.0, y1 = c[1] / 1000.0, x2 = c[2] / 1000.0, y2 = c[3] / 1000.0;
    double lo = 0.0, hi = 1.0, t = x;
    for (int i = 0; i < 40; i++)
    {
        t = (lo + hi) / 2.0;
        double bx = 3.0 * (1.0 - t) * (1.0 - t) * t * x1 + 3.0 * (1.0 - t) * t * t * x2 + t * t * t;
        if (bx < x)
            lo = t;
        else
            hi = t;
    }
    return 3.0 * (1.0 - t) * (1.0 - t) * t * y1 + 3.0 * (1.0 - t) * t * t * y2 + t * t * t;
}

/* The easing's number (1 ...), made when it is new */
static uint32_t easing(writer_t* w, const int16_t* curve)
{
    for (uint32_t i = 0; i < w->easing_count; i++)
    {
        if (memcmp(w->easings[i].curve, curve, sizeof(w->easings[i].curve)) == 0)
            return i + 1U;
    }
    if (w->easing_count >= MAX_EASINGS)
        return 1U;                      /* Out of room: the first one */
    easing_t* e = &w->easings[w->easing_count];
    memcpy(e->curve, curve, sizeof(e->curve));
    for (uint32_t k = 0; k <= EASE_STEPS; k++)
    {
        double y = bezier_at(curve, (double)k / EASE_STEPS);
        e->y[k] = (int32_t)(y * 1000.0 + ((y >= 0.0) ? 0.5 : -0.5));
    }
    return ++w->easing_count;
}

/* "        SET     $<a><suffix>, <b>" */
static void set_op(text_t* t, const char* mnemonic, const char* a, const char* suffix, const char* b)
{
    static const char spaces[] = "        ";
    size_t n = strlen(mnemonic);
    text_add(t, spaces, 8);
    text_add(t, mnemonic, n);
    text_add(t, spaces, (n < 8U) ? 8U - n : 1U);
    text_fmt(t, "$%s%s, %s\n", a, suffix, b);
}

static bool is_text_var(const writer_t* w, dmvsi_var_t var)
{
    dmvsi_var_info_t info;
    return dmvsi_var_info(w->doc, var, &info) == 0 && info.kind == DMVSI_VAR_TEXT;
}

/* An action's operand as the view writes it: $variable, a number (a bound variable's in its view value), "text" */
static void operand(const writer_t* w, const dmvsi_action_t* a, text_t* out)
{
    if (a->operand != 0)
        text_fmt(out, "$%s", var_name(w, a->operand));
    else if (is_text_var(w, a->var) || a->kind == DMVSI_ACT_FORMAT)
        text_string(out, (a->text != NULL) ? a->text : "", (a->text != NULL) ? strlen(a->text) : 0);
    else
        text_fmt(out, "%d", (int)view_value(w, a->var, a->value));
}

/* "        <mnemonic> $<var>, <operand>" */
static void var_op(writer_t* w, text_t* t, const char* mnemonic, const dmvsi_action_t* a)
{
    static const char spaces[] = "        ";
    size_t n = strlen(mnemonic);
    text_add(t, spaces, 8);
    text_add(t, mnemonic, n);
    text_add(t, spaces, (n < 8U) ? 8U - n : 1U);
    text_fmt(t, "$%s, ", var_name(w, a->var));
    operand(w, a, t);
    text_str(t, "\n");
}

typedef struct
{
    char        kind;                       /* 'I' an IF, 'E' an IF past its ELSE, 'L' a LOOP */
    uint32_t    label;
} block_t;

#define MAX_BLOCKS  32u

/*
 * A handler as a subroutine h<n>: IF jumps over what does not hold (to .i<n>,
 * its ELSE's or its END's; past an ELSE, its END is .f<n>), a LOOP is .l<n>
 * ... JMP .l<n>, .b<n>
 */
static void write_handler(writer_t* w, text_t* t, dmvsi_handler_t h)
{
    const dmvsi_action_t* actions = NULL;
    uint32_t count = dmvsi_handler_actions(w->doc, h, &actions);
    block_t stack[MAX_BLOCKS];
    uint32_t depth = 0, labels = 0;
    char value[24];
    text_fmt(t, "\nh%u:\n", (unsigned)h);
    for (uint32_t i = 0; i < count; i++)
    {
        const dmvsi_action_t* a = &actions[i];
        const char* name = (a->var != 0) ? var_name(w, a->var) : "";
        switch (a->kind)
        {
            case DMVSI_ACT_ANIMATE:
                if (a->duration > 0)
                {
                    char n[48];
                    Dmod_SnPrintf(value, sizeof(value), "%d", (int)view_value(w, a->var, a->value));
                    Dmod_SnPrintf(n, sizeof(n), "$%s", name);
                    set_op(t, "SET", name, "_from", n);
                    set_op(t, "SET", name, "_to", value);
                    set_op(t, "SET", name, "_t0", "$time");
                    Dmod_SnPrintf(n, sizeof(n), "%u", (unsigned)a->duration);
                    set_op(t, "SET", name, "_dur", n);
                    Dmod_SnPrintf(n, sizeof(n), "%u", (unsigned)easing(w, a->easing));
                    set_op(t, "SET", name, "_ease", n);
                    set_op(t, "SET", name, "_on", "1");
                    break;
                }
                /* fall through: no time, at once */
            case DMVSI_ACT_SET:
                var_op(w, t, "SET", a);
                if (w->vars[a->var - 1U].animated)
                    set_op(t, "SET", name, "_on", "0");     /* It stops moving */
                break;
            case DMVSI_ACT_TOGGLE:
                text_fmt(t, "        TOGGLE  $%s\n", name);
                break;
            case DMVSI_ACT_ADD: var_op(w, t, "ADD", a); break;
            case DMVSI_ACT_SUB: var_op(w, t, "SUB", a); break;
            case DMVSI_ACT_MUL: var_op(w, t, "MUL", a); break;
            case DMVSI_ACT_DIV: var_op(w, t, "DIV", a); break;
            case DMVSI_ACT_MOD: var_op(w, t, "MOD", a); break;
            case DMVSI_ACT_MIN: var_op(w, t, "MIN", a); break;
            case DMVSI_ACT_MAX: var_op(w, t, "MAX", a); break;
            case DMVSI_ACT_APPEND: var_op(w, t, "APPEND", a); break;
            case DMVSI_ACT_FORMAT:
                text_fmt(t, "        FORMAT  $%s, ", name);
                text_string(t, a->text, strlen(a->text));
                if (a->operand != 0)
                    text_fmt(t, ", $%s\n", var_name(w, a->operand));
                else
                    text_fmt(t, ", %d\n", (int)a->value);
                break;
            case DMVSI_ACT_IF_EQ: case DMVSI_ACT_IF_NE: case DMVSI_ACT_IF_LT:
            case DMVSI_ACT_IF_LE: case DMVSI_ACT_IF_GT: case DMVSI_ACT_IF_GE:
            {
                /* Over the block when it does not hold */
                const char* skip = (a->kind == DMVSI_ACT_IF_EQ) ? "JNE" : (a->kind == DMVSI_ACT_IF_NE) ? "JEQ" :
                                   (a->kind == DMVSI_ACT_IF_LT) ? "JGE" : (a->kind == DMVSI_ACT_IF_LE) ? "JGT" :
                                   (a->kind == DMVSI_ACT_IF_GT) ? "JLE" : "JLT";
                uint32_t label = ++labels;
                if (depth < MAX_BLOCKS)
                {
                    stack[depth].kind = 'I';
                    stack[depth++].label = label;
                }
                text_fmt(t, "        %s     $%s, ", skip, name);
                operand(w, a, t);
                text_fmt(t, ", .i%u\n", (unsigned)label);
                break;
            }
            case DMVSI_ACT_ELSE:
                if (depth > 0)
                {
                    text_fmt(t, "        JMP     .f%u\n.i%u:\n", (unsigned)stack[depth - 1U].label,
                             (unsigned)stack[depth - 1U].label);
                    stack[depth - 1U].kind = 'E';
                }
                break;
            case DMVSI_ACT_LOOP:
            {
                uint32_t label = ++labels;
                if (depth < MAX_BLOCKS)
                {
                    stack[depth].kind = 'L';
                    stack[depth++].label = label;
                }
                text_fmt(t, ".l%u:\n", (unsigned)label);
                break;
            }
            case DMVSI_ACT_BREAK:
            case DMVSI_ACT_CONTINUE:
                for (uint32_t k = depth; k > 0; k--)
                {
                    if (stack[k - 1U].kind == 'L')
                    {
                        text_fmt(t, "        JMP     .%c%u\n", (a->kind == DMVSI_ACT_BREAK) ? 'b' : 'l',
                                 (unsigned)stack[k - 1U].label);
                        break;
                    }
                }
                break;
            case DMVSI_ACT_CALL:
                text_fmt(t, "        CALL    h%u\n", (unsigned)a->handler);
                break;
            case DMVSI_ACT_RETURN:
                text_str(t, "        RET\n");
                break;
            default:                                    /* END */
                if (depth == 0)
                    break;
                depth--;
                if (stack[depth].kind == 'L')
                    text_fmt(t, "        JMP     .l%u\n.b%u:\n", (unsigned)stack[depth].label, (unsigned)stack[depth].label);
                else
                    text_fmt(t, ".%c%u:\n", (stack[depth].kind == 'E') ? 'f' : 'i', (unsigned)stack[depth].label);
                break;
        }
    }
    text_str(t, "        RET\n");
}

/* The most CALLs nested from a handler on (an unmade one calls nothing); 0xFF: past the limit */
static uint32_t call_depth(const writer_t* w, dmvsi_handler_t h, uint32_t depth)
{
    if (depth > MAX_CALLS)
        return 0xFFu;
    const dmvsi_action_t* actions = NULL;
    uint32_t count = dmvsi_handler_actions(w->doc, h, &actions), deepest = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        if (actions[i].kind != DMVSI_ACT_CALL)
            continue;
        uint32_t d = 1U + call_depth(w, actions[i].handler, depth + 1U);
        if (d > deepest)
            deepest = d;
    }
    return deepest;
}

/* Every animated variable moved toward its target, each frame */
static void write_animations(writer_t* w, text_t* t)
{
    text_str(t, "\n; Every running animation one frame further: progress 0 ... 1000, eased\nanimate:\n");
    for (uint32_t i = 0; i < w->var_count; i++)
    {
        if (!w->vars[i].animated)
            continue;
        const char* v = var_name(w, (dmvsi_var_t)(i + 1U));
        text_fmt(t, "        JEQ     $%s_on, 0, .n%u\n", v, (unsigned)i);
        text_str(t, "        SET     $anim_p, $time\n");
        text_fmt(t, "        SUB     $anim_p, $%s_t0\n        MUL     $anim_p, 1000\n", v);
        text_fmt(t, "        DIV     $anim_p, $%s_dur\n        CLAMP   $anim_p, 0, 1000\n", v);
        for (uint32_t k = 1; k <= w->easing_count; k++)
        {
            text_fmt(t, "        JNE     $%s_ease, %u, .e%u_%u\n        CALL    ease%u\n.e%u_%u:\n", v, (unsigned)k,
                     (unsigned)i, (unsigned)k, (unsigned)k, (unsigned)i, (unsigned)k);
        }
        text_fmt(t, "        SET     $%s, $%s_to\n        SUB     $%s, $%s_from\n", v, v, v, v);
        text_fmt(t, "        MUL     $%s, $anim_e\n        DIV     $%s, 1000\n        ADD     $%s, $%s_from\n", v, v, v, v);
        text_fmt(t, "        JLT     $anim_p, 1000, .n%u\n        SET     $%s_on, 0\n.n%u:\n", (unsigned)i, v, (unsigned)i);
    }
    text_str(t, "        RET\n");

    /* Each easing: its curve as EASE_STEPS lines */
    for (uint32_t k = 0; k < w->easing_count; k++)
    {
        const easing_t* e = &w->easings[k];
        text_fmt(t, "\n; cubic-bezier(%d, %d, %d, %d) / 1000: $anim_p -> $anim_e\nease%u:\n", (int)e->curve[0],
                 (int)e->curve[1], (int)e->curve[2], (int)e->curve[3], (unsigned)(k + 1U));
        for (uint32_t s = 0; s < EASE_STEPS; s++)
        {
            int32_t x0 = (int32_t)(s * 1000U / EASE_STEPS), x1 = (int32_t)((s + 1U) * 1000U / EASE_STEPS);
            text_fmt(t, "        JGE     $anim_p, %d, .s%u\n", (int)x1, (unsigned)s);
            text_fmt(t, "        SET     $anim_e, $anim_p\n        SUB     $anim_e, %d\n        MUL     $anim_e, %d\n", (int)x0,
                     (int)(e->y[s + 1U] - e->y[s]));
            text_fmt(t, "        DIV     $anim_e, %d\n        ADD     $anim_e, %d\n        RET\n.s%u:\n", (int)(x1 - x0),
                     (int)e->y[s], (unsigned)s);
        }
        text_str(t, "        SET     $anim_e, 1000\n        RET\n");
    }
}

/* The variables (after the code: the positions are known), the timers, the handler run first */
static void declare_behaviour(writer_t* w, text_t* t)
{
    uint16_t ms = 0;
    dmvsi_handler_t h = 0;
    bool timers = dmvsi_timer_at(w->doc, 0, &ms, &h);
    if (w->var_count == 0 && !timers && dmvsi_init_handler(w->doc) == 0)
        return;
    text_str(t, "\n");
    for (uint32_t i = 0; i < w->var_count; i++)
    {
        dmvsi_var_info_t info;
        (void)dmvsi_var_info(w->doc, (dmvsi_var_t)(i + 1U), &info);
        if (info.kind == DMVSI_VAR_TEXT)
        {
            text_fmt(t, ".var    $%s, str[%u], ", info.name, (unsigned)info.size);
            text_string(t, info.text, strlen(info.text));
            text_str(t, "\n");
            continue;
        }
        text_fmt(t, ".var    $%s, int, %d\n", info.name, (int)view_value(w, (dmvsi_var_t)(i + 1U), info.initial));
        if (w->vars[i].animated)
        {
            static const char suffixes[6][6] = { "_from", "_to", "_t0", "_dur", "_ease", "_on" };
            for (uint32_t k = 0; k < 6U; k++)
                text_fmt(t, ".var    $%s%s, int, 0\n", info.name, suffixes[k]);
        }
    }
    if (w->animates)
        text_str(t, ".var    $anim_p, int, 0\n.var    $anim_e, int, 0\n.timer  16, animate\n");
    for (uint32_t i = 0; dmvsi_timer_at(w->doc, i, &ms, &h); i++)
        text_fmt(t, ".timer  %u, h%u\n", (unsigned)ms, (unsigned)h);
    if (dmvsi_init_handler(w->doc) != 0)
        text_fmt(t, ".init   h%u\n", (unsigned)dmvsi_init_handler(w->doc));
}

/* The variables the handlers animate */
static void find_animations(writer_t* w)
{
    const dmvsi_action_t* actions = NULL;
    for (dmvsi_handler_t h = 1; ; h++)
    {
        uint32_t count = dmvsi_handler_actions(w->doc, h, &actions);
        if (count == 0 && actions == NULL)
            break;
        for (uint32_t i = 0; i < count; i++)
        {
            if (actions[i].kind == DMVSI_ACT_ANIMATE && actions[i].duration > 0 && actions[i].var <= w->var_count)
            {
                w->vars[actions[i].var - 1U].animated = true;
                w->animates = true;
            }
        }
        actions = NULL;
    }
}

/* ---- The view ---- */

static int write_view(writer_t* w, const char* output, const char* source)
{
    uint16_t width = 0, height = 0;
    char view[MAX_NAME];
    text_t t = { 0 };
    const dmvsi_node_t* root = dmvsi_root(w->doc);
    if (root == NULL || dmvsi_view_size(w->doc, &width, &height) != 0)
        return -EINVAL;
    identifier(dmvsi_view_name(w->doc), "view_", view, sizeof(view));

    int ret = name_fonts(w);
    if (ret != 0)
        return ret;
    while (dmvsi_var_at(w->doc, w->var_count, NULL, NULL))
        w->var_count++;
    if (w->var_count > 0)
    {
        if ((w->vars = Dmod_Malloc(w->var_count * sizeof(var_info_t))) == NULL)
            return -ENOMEM;
        memset(w->vars, 0, w->var_count * sizeof(var_info_t));
    }
    find_animations(w);
    for (dmvsi_handler_t h = 1; ; h++)
    {
        const dmvsi_action_t* actions = NULL;
        if (dmvsi_handler_actions(w->doc, h, &actions) == 0 && actions == NULL)
            break;
        if (call_depth(w, h, 0) >= MAX_CALLS)
        {
            DMOD_LOG_ERROR("todmvs: handler %u calls more than %u deep (dmview's stack)\n", (unsigned)h, (unsigned)MAX_CALLS);
            return -E2BIG;
        }
    }
    const dmvsi_node_t* previous = NULL;
    find_merges(w, root->first, &previous);
    box_t screen = { 0, 0, width, height };
    write_nodes(w, root->first, &screen, &screen);
    if (w->error != 0)
        return w->error;
    w->r.gradients = w->gradient_count;
    for (uint32_t i = 0; i < w->font_count; i++)
        w->r.fonts += w->uses[i].used ? 1U : 0U;

    text_fmt(&t, "; %s - made by todmvs", view);
    if (source != NULL)
        text_fmt(&t, " from %s", base_name(source));
    if (dmvsi_converter_name(w->doc) != NULL)
        text_fmt(&t, " (%s)", dmvsi_converter_name(w->doc));
    text_fmt(&t, "\n\n.view   %s\n.size   %u, %u\n.entry  draw\n", view, (unsigned)width, (unsigned)height);
    if (w->r.fonts != 0)
        text_str(&t, "\n");
    for (uint32_t i = 0; i < w->font_count; i++)
    {
        char spec[MAX_NAME];
        if (!w->uses[i].used)
            continue;               /* Of text that is not drawn (off the screen) */
        font_spec(dmvsi_font_at(w->doc, i), spec, sizeof(spec));
        text_fmt(&t, ".font   %s, \"%s\"\n", w->fonts[i], spec);
    }
    declare_gradients(w, &t);
    declare_behaviour(w, &t);
    text_str(&t, "\ndraw:\n");
    if (w->code.size != 0)
        text_add(&t, w->code.data, w->code.size);
    text_str(&t, "        RET\n");
    for (dmvsi_handler_t h = 1; ; h++)
    {
        const dmvsi_action_t* actions = NULL;
        if (dmvsi_handler_actions(w->doc, h, &actions) == 0 && actions == NULL)
            break;
        write_handler(w, &t, h);
    }
    if (w->animates)
        write_animations(w, &t);
    ret = (t.failed || w->code.failed) ? -ENOMEM : text_save(&t, output) ? 0 : -EIO;
    text_free(&t);
    return ret;
}

/* ---- API ---- */

dmod_libtodmvs_api_declaration(1.0, int, _write, ( dmvsi_doc_t doc, const char* output, const libtodmvs_options_t* options, libtodmvs_result_t* result ))
{
    static const libtodmvs_options_t defaults = { 0 };
    char dir[256];
    if (doc == NULL || output == NULL)
        return -EINVAL;
    if (options == NULL)
        options = &defaults;

    writer_t* w = Dmod_Malloc(sizeof(*w));
    if (w == NULL)
        return -ENOMEM;
    memset(w, 0, sizeof(*w));
    w->doc = doc;
    w->boxes = Dmod_Malloc(MAX_BOX_NAMES * MAX_NAME);
    dir_of(output, dir, sizeof(dir));
    w->dir = dir;
    w->no_assets = options->no_assets;

    int ret = (w->boxes == NULL) ? -ENOMEM : write_view(w, output, options->source);
    if (ret == 0 && !options->no_assets)
        ret = write_fonts(doc, dir, dmvsi_view_name(doc), w->uses);
    if (result != NULL)
        *result = w->r;

    text_free(&w->code);
    Dmod_Free(w->gradients);
    Dmod_Free(w->fonts);
    uses_free(w->uses, w->font_count);
    Dmod_Free(w->vars);
    Dmod_Free(w->boxes);
    Dmod_Free(w);
    return ret;
}

dmod_libtodmvs_api_declaration(1.0, int, _convert_file, ( const char* input, const char* output, const dmvsi_options_t* convert,
                                                          const libtodmvs_options_t* options, libtodmvs_result_t* result ))
{
    libtodmvs_options_t o = { 0 };
    int status = 0;
    if (input == NULL || output == NULL)
        return -EINVAL;
    dmvsi_doc_t doc = dmvsi_convert_file(input, convert, &status);
    if (doc == NULL)
        return (status != 0) ? status : -EBADMSG;
    if (options != NULL)
        o = *options;
    if (o.source == NULL)
        o.source = input;
    int ret = libtodmvs_write(doc, output, &o, result);
    dmvsi_free(doc);
    return ret;
}

/* ---- Module ---- */

int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}
