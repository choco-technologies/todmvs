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
    box_t all = { 0, 0, 0, 0 };
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

static void write_text(writer_t* w, const dmvsi_text_t* t, const box_t* origin)
{
    box_t b = text_box(t);
    int32_t v[4] = { b.x0 - origin->x0, b.y0 - origin->y0, b.x1 - b.x0, b.y1 - b.y0 };
    op(w, "TEXT");
    numbers(w, v, 4);
    text_str(&w->code, ", ");
    text_string(&w->code, t->text, t->length);
    text_fmt(&w->code, ", %s, ", font_name(w, t->font, t->text, t->length));
    paint(w, &t->paint);
    text_str(&w->code, ", LEFT|TOP\n");
}

/* The image next to the view, named as the .dmvi dmod makes of it */
static void write_image(writer_t* w, const dmvsi_image_t* im, const box_t* origin)
{
    char name[256], copy[512];
    const char* base = base_name(im->path);
    const char* dot = strrchr(base, '.');
    size_t stem = (dot != NULL && dot != base) ? (size_t)(dot - base) : strlen(base);
    if (stem + sizeof(".dmvi") > sizeof(name))
        stem = sizeof(name) - sizeof(".dmvi");
    memcpy(name, base, stem);
    strcpy(name + stem, ".dmvi");
    if (!w->no_assets && w->error == 0)
    {
        Dmod_SnPrintf(copy, sizeof(copy), "%s/%s", w->dir, base);
        int ret = copy_file(im->path, copy);
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
    text_string(&w->code, name, strlen(name));
    if (mask)
    {
        text_str(&w->code, ", ");
        paint(w, &im->paint);
    }
    text_str(&w->code, ", LEFT|TOP\n");
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

static void write_nodes(writer_t* w, const dmvsi_node_t* first, const box_t* origin, const box_t* clip);

static void write_group(writer_t* w, const dmvsi_node_t* n, const box_t* origin, const box_t* clip)
{
    const dmvsi_group_t* g = &n->u.group;
    bool clips = (g->flags & DMVSI_GROUP_CLIP) != 0;
    bool scrolls = g->scroll_w > 0 && g->scroll_h > 0;
    if (g->opacity == 0)
    {
        w->r.skipped++;
        return;
    }
    if (!clips && g->opacity == 255 && !scrolls)
    {
        write_nodes(w, n->first, origin, clip);
        return;
    }

    box_t rect = clips ? snap_rect(&g->rect) : extent(n);
    int32_t v[4] = { rect.x0 - origin->x0, rect.y0 - origin->y0, rect.x1 - rect.x0, rect.y1 - rect.y0 };
    op(w, "BOX");
    text_fmt(&w->code, "@%s, ", box_name(w, g->name));
    numbers(w, v, 4);
    if (group_opaque(n, &rect))
        text_str(&w->code, ", OPAQUE");
    text_str(&w->code, "\n");
    w->r.boxes++;
    box_t inside = box_and(*clip, rect);
    if (scrolls)
    {
        int32_t s[2] = { snap(g->scroll_w), snap(g->scroll_h) };
        op(w, "SCROLL");
        numbers(w, s, 2);
        text_str(&w->code, "\n");
        box_t content = { rect.x0, rect.y0, rect.x0 + s[0], rect.y0 + s[1] };
        inside = content;               /* What can be scrolled into view */
    }
    if (g->opacity != 255)
    {
        op(w, "OPACITY");
        text_fmt(&w->code, "%u\n", (unsigned)g->opacity);
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
        if (box_empty(&seen))
        {
            w->r.skipped++;
            continue;
        }
        switch (n->kind)
        {
            case DMVSI_NODE_GROUP:
                write_group(w, n, origin, clip);
                continue;
            case DMVSI_NODE_RECT:
                if (!paint_visible(&n->u.fill.paint))
                    break;
                write_fill(w, &n->u.fill, origin);
                w->r.instructions++;
                continue;
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
    text_str(&t, "\ndraw:\n");
    if (w->code.size != 0)
        text_add(&t, w->code.data, w->code.size);
    text_str(&t, "        RET\n");
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
