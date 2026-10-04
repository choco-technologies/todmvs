#include "private.h"
#include <string.h>

/*
 * Shadows: dmview draws no blur, so a shadow is painted with gradients -
 * the falloff of the blur as their stops - on pieces around the shape.
 *
 * Outer shadows of a rounded rectangle S (radius r): the blur is taken as a
 * function of the distance u from the core K - S shrunk by r - so the
 * intensity is Phi((r - u) / sigma) (the Gaussian's integral), scaled so
 * the middle of the shape is what a blurred rectangle has there (a small
 * shape blurred much is fainter). Around K are eight pieces: four bands
 * whose gradient goes straight out, four corners with a radial one around
 * K's corners; K itself is one color. Of every piece only what lies
 * outside the hole (the element casting the shadow) is painted, and
 * nothing that would be invisible.
 *
 * A blurred circle (a round element with blur(), the shadow of a round
 * button) is one CIRCLE with a radial gradient: the exact intensity of a
 * blurred disk - the Gaussian integrated over it, numerically.
 *
 * Inset shadows are four strips along the inside of the hole, each with the
 * falloff from its edge in.
 *
 * Doubles without libm: exp, erf and sqrt are approximated here.
 */

#define SIGMAS          3.0             /* The blur reaches this far, in standard deviations */
#define MIN_SIGMA       0.25            /* A shadow without blur: a sharp edge */
#define BAND_STOPS      8u
#define CORNER_STOPS    10u
#define CIRCLE_STOPS    12u
#define CIRCLE_STEPS    64              /* Of the numeric integral (even, Simpson) */
#define MIN_ALPHA       2u              /* A piece fainter than this everywhere is left out */

/* ---- Math ---- */

static double fabs_(double x) { return (x < 0.0) ? -x : x; }

static double exp_(double x)
{
    const double ln2 = 0.69314718055994531;
    if (x < -700.0)
        return 0.0;
    if (x > 700.0)
        x = 700.0;
    int k = (int)((x / ln2) + ((x >= 0.0) ? 0.5 : -0.5));
    double r = x - (double)k * ln2, term = 1.0, sum = 1.0;
    for (int i = 1; i < 14; i++)
    {
        term *= r / (double)i;
        sum += term;
    }
    for (; k > 0; k--)
        sum *= 2.0;
    for (; k < 0; k++)
        sum *= 0.5;
    return sum;
}

static double sqrt_(double x)
{
    if (x <= 0.0)
        return 0.0;
    double y = (x > 1.0) ? x : 1.0;
    for (int i = 0; i < 60; i++)
    {
        double next = 0.5 * (y + x / y);
        if (fabs_(next - y) < 1e-12 * y)
            return next;
        y = next;
    }
    return y;
}

/* Abramowitz & Stegun 7.1.26 - to 1.5e-7 */
static double erf_(double x)
{
    double s = (x < 0.0) ? -1.0 : 1.0;
    x = fabs_(x);
    double t = 1.0 / (1.0 + 0.3275911 * x);
    double y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t *
                     exp_(-x * x);
    return s * y;
}

/* The Gaussian's integral up to x standard deviations */
static double phi(double x)
{
    return 0.5 * (1.0 + erf_(x * 0.70710678118654752));
}

static int32_t iround(double x)
{
    return (int32_t)((x >= 0.0) ? x + 0.5 : x - 0.5);
}

/* How far the blur reaches: 3 sigma, rounded up */
static int32_t blur_reach(double sigma)
{
    double reach = SIGMAS * sigma;
    int32_t e = (int32_t)reach;
    return ((double)e < reach) ? e + 1 : e;
}

static uint32_t with_alpha(uint32_t color, double alpha)
{
    int32_t a = iround(alpha);
    a = (a < 0) ? 0 : (a > 255) ? 255 : a;
    return ((uint32_t)a << 24) | (color & 0x00FFFFFFu);
}

/* ---- Pieces ---- */

typedef struct
{
    double      alpha;              /* Of the shadow's color */
    uint32_t    color;
    double      sigma;
    double      r;                  /* Outer: the shape's radius */
    double      scale;              /* Outer: of the falloff, so the middle is a blurred rectangle's */
} falloff_t;

/* Outer: at distance u from the core */
static double outer(const falloff_t* f, double u)
{
    return f->alpha * f->scale * phi((f->r - u) / f->sigma);
}

typedef struct
{
    piece_t*    pieces;
    uint32_t    count;
} list_t;

static piece_t* add_piece(list_t* l, uint8_t kind, box_t box)
{
    if (l->count >= MAX_PIECES || box_empty(&box))
        return NULL;
    piece_t* p = &l->pieces[l->count++];
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->box = box;
    return p;
}

/* The stops' alphas checked: a piece too faint to see is taken back */
static void keep_if_visible(list_t* l, const piece_t* p)
{
    uint32_t most = (p->paint.kind == DMVSI_PAINT_COLOR) ? (p->paint.color >> 24) : 0;
    for (uint32_t i = 0; i < p->paint.count; i++)
    {
        uint32_t a = p->paint.stops[i].color >> 24;
        most = (a > most) ? a : most;
    }
    if (most < MIN_ALPHA)
        l->count--;
}

/* `z` without `hole`: up to four rectangles */
static uint32_t minus(box_t z, box_t hole, box_t* out)
{
    box_t h = box_and(z, hole);
    if (box_empty(&z))
        return 0;
    if (box_empty(&h))
    {
        out[0] = z;
        return 1;
    }
    uint32_t n = 0;
    box_t top = { z.x0, z.y0, z.x1, h.y0 }, bottom = { z.x0, h.y1, z.x1, z.y1 };
    box_t left = { z.x0, h.y0, h.x0, h.y1 }, right = { h.x1, h.y0, z.x1, h.y1 };
    if (!box_empty(&top))
        out[n++] = top;
    if (!box_empty(&bottom))
        out[n++] = bottom;
    if (!box_empty(&left))
        out[n++] = left;
    if (!box_empty(&right))
        out[n++] = right;
    return n;
}

/* A linear piece: the gradient goes along `angle`, from u0 at its start to u1 at its end */
static void linear_piece(list_t* l, const falloff_t* f, box_t b, int16_t angle, double u0, double u1, bool inset)
{
    piece_t* p = add_piece(l, PIECE_RECT, b);
    if (p == NULL)
        return;
    int32_t span = (angle == 0 || angle == 180) ? b.y1 - b.y0 : b.x1 - b.x0;
    uint32_t n = (span + 1 < (int32_t)BAND_STOPS) ? (uint32_t)((span + 1 < 2) ? 2 : span + 1) : BAND_STOPS;
    p->paint.kind = DMVSI_PAINT_LINEAR;
    p->paint.angle = angle;
    p->paint.count = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++)
    {
        double t = (double)i / (double)(n - 1U);
        double u = u0 + (u1 - u0) * t;
        double a = inset ? f->alpha * phi(u / f->sigma) : outer(f, u);
        p->paint.stops[i].color = with_alpha(f->color, a);
        p->paint.stops[i].position = (uint16_t)iround(t * 10000.0);
    }
    keep_if_visible(l, p);
}

/* A corner piece: radial around (cx, cy), out to `reach` */
static void corner_piece(list_t* l, const falloff_t* f, box_t b, double cx, double cy, double reach)
{
    piece_t* p = add_piece(l, PIECE_RECT, b);
    if (p == NULL)
        return;
    double w = (double)(b.x1 - b.x0), h = (double)(b.y1 - b.y0);
    p->paint.kind = DMVSI_PAINT_RADIAL;
    p->paint.cx = iround((cx - (double)b.x0) / w * 10000.0);
    p->paint.cy = iround((cy - (double)b.y0) / h * 10000.0);
    p->paint.rx = iround(reach / w * 10000.0);
    p->paint.ry = iround(reach / h * 10000.0);
    p->paint.count = (uint8_t)CORNER_STOPS;
    for (uint32_t i = 0; i < CORNER_STOPS; i++)
    {
        double t = (double)i / (double)(CORNER_STOPS - 1U);
        double a = (i + 1U == CORNER_STOPS) ? 0.0 : outer(f, reach * t);
        p->paint.stops[i].color = with_alpha(f->color, a);
        p->paint.stops[i].position = (uint16_t)iround(t * 10000.0);
    }
    if (p->paint.rx <= 0 || p->paint.ry <= 0)
        l->count--;
    else
        keep_if_visible(l, p);
}

/* A disk of radius R blurred by sigma, at distance d from its center: the
 * Gaussian over every column of the disk, numerically (Simpson) */
static double blurred_disk(double radius, double sigma, double d)
{
    double step = 2.0 * radius / (double)CIRCLE_STEPS, sum = 0.0;
    for (int i = 0; i <= CIRCLE_STEPS; i++)
    {
        double x = -radius + step * (double)i;
        double chord = sqrt_(radius * radius - x * x);
        double g = exp_(-(x - d) * (x - d) / (2.0 * sigma * sigma)) / (sigma * 2.50662827463100050);
        double v = g * erf_(chord / (sigma * 1.41421356237309505));
        sum += v * ((i == 0 || i == CIRCLE_STEPS) ? 1.0 : (i % 2 != 0) ? 4.0 : 2.0);
    }
    return sum * step / 3.0;
}

static void circle(list_t* l, double alpha, uint32_t color, double cx, double cy, double radius, double sigma)
{
    double reach = radius + SIGMAS * sigma;
    int32_t r = iround(reach), x = iround(cx), y = iround(cy);
    box_t b = { x - r, y - r, x + r, y + r };
    piece_t* p = add_piece(l, PIECE_CIRCLE, b);
    if (p == NULL)
        return;
    p->paint.kind = DMVSI_PAINT_RADIAL;
    p->paint.cx = p->paint.cy = p->paint.rx = p->paint.ry = DMVSI_PERCENT(50);
    p->paint.count = (uint8_t)CIRCLE_STOPS;
    for (uint32_t i = 0; i < CIRCLE_STOPS; i++)
    {
        double t = (double)i / (double)(CIRCLE_STOPS - 1U);
        double a = (i + 1U == CIRCLE_STOPS) ? 0.0 : alpha * blurred_disk(radius, sigma, (double)r * t);
        p->paint.stops[i].color = with_alpha(color, a);
        p->paint.stops[i].position = (uint16_t)iround(t * 10000.0);
    }
    keep_if_visible(l, p);
}

static void outer_shadow(list_t* l, const dmvsi_shadow_t* s, double alpha, double sigma)
{
    box_t S = snap_rect(&s->shape), H = snap_rect(&s->hole);
    double w = (double)(S.x1 - S.x0), h = (double)(S.y1 - S.y0);
    double r = (double)s->radius / DMVSI_UNIT;
    double half = ((w < h) ? w : h) / 2.0;
    if (w <= 0.0 || h <= 0.0)
        return;
    if (r > half)
        r = half;
    if (r >= half - 0.5 && fabs_(w - h) <= 1.0)
    {
        circle(l, alpha, s->color, (double)S.x0 + w / 2.0, (double)S.y0 + h / 2.0, half, sigma);
        return;
    }

    int32_t ri = iround(r), e = blur_reach(sigma), reach = ri + e;
    box_t K = { S.x0 + ri, S.y0 + ri, S.x1 - ri, S.y1 - ri };
    falloff_t f = { alpha, s->color, sigma, r, 1.0 };
    double sq = sigma * 2.82842712474619010;        /* 2 sqrt(2) sigma */
    f.scale = erf_(w / sq) * erf_(h / sq) / phi(r / sigma);

    box_t pieces[4];
    box_t center = K;
    uint32_t n = minus(center, H, pieces);
    for (uint32_t i = 0; i < n; i++)
    {
        piece_t* p = add_piece(l, PIECE_RECT, pieces[i]);
        if (p != NULL)
        {
            p->paint.color = with_alpha(s->color, outer(&f, 0.0));
            keep_if_visible(l, p);
        }
    }

    /* The bands: top, bottom, left, right */
    box_t bands[4] = {
        { K.x0, K.y0 - reach, K.x1, K.y0 }, { K.x0, K.y1, K.x1, K.y1 + reach },
        { K.x0 - reach, K.y0, K.x0, K.y1 }, { K.x1, K.y0, K.x1 + reach, K.y1 },
    };
    for (uint32_t b = 0; b < 4U; b++)
    {
        n = minus(bands[b], H, pieces);
        for (uint32_t i = 0; i < n; i++)
        {
            box_t p = pieces[i];
            switch (b)
            {
                case 0: linear_piece(l, &f, p, 0, (double)(K.y0 - p.y1), (double)(K.y0 - p.y0), false); break;
                case 1: linear_piece(l, &f, p, 180, (double)(p.y0 - K.y1), (double)(p.y1 - K.y1), false); break;
                case 2: linear_piece(l, &f, p, 270, (double)(K.x0 - p.x1), (double)(K.x0 - p.x0), false); break;
                default: linear_piece(l, &f, p, 90, (double)(p.x0 - K.x1), (double)(p.x1 - K.x1), false); break;
            }
        }
    }

    /* The corners, around K's corners */
    const int32_t corners[4][2] = { { K.x0, K.y0 }, { K.x1, K.y0 }, { K.x0, K.y1 }, { K.x1, K.y1 } };
    for (uint32_t c = 0; c < 4U; c++)
    {
        int32_t cx = corners[c][0], cy = corners[c][1];
        box_t z = { (c % 2U == 0U) ? cx - reach : cx, (c < 2U) ? cy - reach : cy,
                    (c % 2U == 0U) ? cx : cx + reach, (c < 2U) ? cy : cy + reach };
        n = minus(z, H, pieces);
        for (uint32_t i = 0; i < n; i++)
            corner_piece(l, &f, pieces[i], (double)cx, (double)cy, (double)reach);
    }
}

static void inset_shadow(list_t* l, const dmvsi_shadow_t* s, double alpha, double sigma)
{
    box_t S = snap_rect(&s->shape), H = snap_rect(&s->hole);
    int32_t e = blur_reach(sigma);
    falloff_t f = { alpha, s->color, sigma, 0.0, 1.0 };
    if (box_empty(&H))
        return;

    int32_t top_end = (S.y0 + e < H.y1) ? S.y0 + e : H.y1;
    int32_t bottom_start = (S.y1 - e > top_end) ? S.y1 - e : top_end;
    int32_t left_end = (S.x0 + e < H.x1) ? S.x0 + e : H.x1;
    int32_t right_start = (S.x1 - e > left_end) ? S.x1 - e : left_end;

    /* The falloff is Phi(distance outside S / sigma): u is that distance */
    box_t top = { H.x0, H.y0, H.x1, top_end }, bottom = { H.x0, bottom_start, H.x1, H.y1 };
    box_t left = { H.x0, top_end, left_end, bottom_start }, right = { right_start, top_end, H.x1, bottom_start };
    if (!box_empty(&top))
        linear_piece(l, &f, top, 180, (double)(S.y0 - top.y0), (double)(S.y0 - top.y1), true);
    if (!box_empty(&bottom))
        linear_piece(l, &f, bottom, 0, (double)(bottom.y1 - S.y1), (double)(bottom.y0 - S.y1), true);
    if (!box_empty(&left))
        linear_piece(l, &f, left, 90, (double)(S.x0 - left.x0), (double)(S.x0 - left.x1), true);
    if (!box_empty(&right))
        linear_piece(l, &f, right, 270, (double)(right.x1 - S.x1), (double)(right.x0 - S.x1), true);
}

uint32_t shadow_pieces(const dmvsi_shadow_t* s, piece_t* pieces)
{
    list_t l = { pieces, 0 };
    double alpha = (double)(s->color >> 24);
    double sigma = (double)s->sigma / DMVSI_UNIT;
    if (alpha < (double)MIN_ALPHA)
        return 0;
    if (sigma < MIN_SIGMA)
        sigma = MIN_SIGMA;
    if ((s->flags & DMVSI_SHADOW_INSET) != 0)
        inset_shadow(&l, s, alpha, sigma);
    else
        outer_shadow(&l, s, alpha, sigma);
    return l.count;
}
