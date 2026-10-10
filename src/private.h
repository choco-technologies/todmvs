#ifndef LIBTODMVS_PRIVATE_H
#define LIBTODMVS_PRIVATE_H

#include "libtodmvs.h"

/* A rectangle in whole pixels: x0, y0 inclusive, x1, y1 exclusive */
typedef struct
{
    int32_t     x0, y0, x1, y1;
} box_t;

static inline bool box_empty(const box_t* b) { return b->x1 <= b->x0 || b->y1 <= b->y0; }

static inline box_t box_and(box_t a, box_t b)
{
    box_t r = { (a.x0 > b.x0) ? a.x0 : b.x0, (a.y0 > b.y0) ? a.y0 : b.y0,
                (a.x1 < b.x1) ? a.x1 : b.x1, (a.y1 < b.y1) ? a.y1 : b.y1 };
    return r;
}

/* Units (1/64 pixel) to the nearest whole pixel */
static inline int32_t snap(dmvsi_unit_t v) { return (v + DMVSI_UNIT / 2) >> 6; }

static inline box_t snap_rect(const dmvsi_rect_t* r)
{
    box_t b = { snap(r->x), snap(r->y), snap(r->x + r->w), snap(r->y + r->h) };
    return b;
}

/* ---- Text in memory (out.c) ---- */

typedef struct
{
    char*       data;
    size_t      size;
    size_t      capacity;
    bool        failed;         /* Out of memory: everything after is dropped */
} text_t;

void    text_free(text_t* t);
void    text_add(text_t* t, const char* s, size_t length);
void    text_str(text_t* t, const char* s);
void    text_fmt(text_t* t, const char* format, ...);
void    text_color(text_t* t, uint32_t color);
void    text_string(text_t* t, const char* s, size_t length);     /* A quoted .dmvs string */
bool    text_save(const text_t* t, const char* path);
bool    text_load(text_t* t, const char* path);                   /* false: no file */

/* ---- A piece of a painted shadow (shadow.c) ---- */

#define PIECE_RECT      0u
#define PIECE_CIRCLE    1u      /* `box` is the square around it */

typedef struct
{
    uint8_t         kind;
    box_t           box;
    dmvsi_paint_t   paint;
} piece_t;

#define MAX_PIECES      40u

/* The pieces that paint a shadow, at most MAX_PIECES; returns how many */
uint32_t shadow_pieces(const dmvsi_shadow_t* shadow, piece_t* pieces);

/* ---- Assets (assets.c) ---- */

/* What the view draws of a font of the document: whether it does, and its characters (increasing) */
typedef struct
{
    bool        used;
    uint32_t*   chars;
    uint32_t    count;
    uint32_t    capacity;
} font_use_t;

int     use_chars(font_use_t* use, const char* text, size_t length);
void    uses_free(font_use_t* uses, uint32_t count);

/* The .font name and spec of a font: "inter-bold-48" */
void    font_spec(dmvsi_font_t font, char* spec, size_t size);
int     write_fonts(dmvsi_doc_t doc, const char* dir, const char* view, const font_use_t* uses);
int     copy_file(const char* from, const char* to);
/* What a view draws of an image: its size, blur, the part of it shown (crop: x, y, w, h; w 0: all), its corners */
typedef struct
{
    uint32_t    width, height;
    uint32_t    blur;
    uint32_t    crop[4];
    uint32_t    radius;
} image_shape_t;

int     image_section(const char* dir, const char* view, const char* image, const char* name,
                      const image_shape_t* shape);                      /* A size of an image in its .ini */
void    dir_of(const char* path, char* dir, size_t size);
const char* base_name(const char* path);

#endif /* LIBTODMVS_PRIVATE_H */
