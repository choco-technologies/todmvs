#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "libtodmvs.h"
#include "libtodmv.h"
#include <errno.h>
#include <string.h>

/*
 * libtodmvs with documents made in code: what the view says, that libtodmv
 * assembles it, the fonts next to it - and the pieces of shadows.
 */

#ifndef LIBTODMVS_TEST_DIR
#define LIBTODMVS_TEST_DIR      "."
#endif
#ifndef LIBTODMVS_TEST_FONTS
#define LIBTODMVS_TEST_FONTS    "fonts"
#endif
#define TEST_FILE(name)         LIBTODMVS_TEST_DIR "/" name
#define INTER                   LIBTODMVS_TEST_FONTS "/Inter-Regular.otf"

static char g_text[16384];

static const char* read_file(const char* path)
{
    g_text[0] = '\0';
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return g_text;
    size_t n = Dmod_FileRead(g_text, 1, sizeof(g_text) - 1U, f);
    Dmod_FileClose(f);
    g_text[n] = '\0';
    return g_text;
}

static bool has(const char* text, const char* what)
{
    size_t n = strlen(what);
    for (const char* p = text; *p != '\0'; p++)
    {
        if (strncmp(p, what, n) == 0)
            return true;
    }
    return false;
}

static void rect(dmvsi_rect_t* r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    r->x = DMVSI_PX(x);
    r->y = DMVSI_PX(y);
    r->w = DMVSI_PX(w);
    r->h = DMVSI_PX(h);
}

static uint32_t g_errors;

static void on_error(void* user, const libtodmv_error_t* e)
{
    (void)user;
    Dmod_Printf("  %u:%u: %s\n", (unsigned)e->line, (unsigned)e->column, e->message);
    g_errors++;
}

/* Whether libtodmv assembles the view */
static bool assembles(const char* dmvs, const char* dmv)
{
    libtodmv_options_t o;
    libtodmv_result_t r;
    memset(&o, 0, sizeof(o));
    o.on_error = on_error;
    g_errors = 0;
    return libtodmv_assemble_file(dmvs, dmv, &o, &r) == 0 && r.error_count == 0 && g_errors == 0;
}

/* A screen: a background, a panel that clips with a gradient and text,
 * something off the screen, a translucent group, a shadow */
static dmvsi_doc_t make_doc(const char* text)
{
    dmvsi_doc_t doc = dmvsi_new();
    dmvsi_fill_t fill;
    dmvsi_group_t group;
    dmvsi_text_t t;
    dmvsi_shadow_t shadow;
    dmvsi_frame_t frame;
    (void)dmvsi_set_view(doc, "demo-view", 100, 80);

    memset(&fill, 0, sizeof(fill));
    rect(&fill.rect, 0, 0, 100, 80);
    fill.paint.color = 0xFF102030u;
    (void)dmvsi_add_fill(doc, &fill);

    memset(&shadow, 0, sizeof(shadow));
    rect(&shadow.shape, 10, 14, 50, 30);
    rect(&shadow.hole, 10, 10, 50, 30);
    shadow.radius = DMVSI_PX(4);
    shadow.hole_radius = DMVSI_PX(4);
    shadow.sigma = DMVSI_PX(3);
    shadow.color = 0x40000000u;
    (void)dmvsi_add_shadow(doc, &shadow);

    memset(&group, 0, sizeof(group));
    rect(&group.rect, 10, 10, 50, 30);
    group.flags = DMVSI_GROUP_CLIP;
    group.opacity = 255;
    group.name = "panel";
    (void)dmvsi_begin_group(doc, &group);
    memset(&fill, 0, sizeof(fill));
    rect(&fill.rect, 10, 10, 50, 30);
    fill.radius = DMVSI_PX(4);
    fill.paint.kind = DMVSI_PAINT_LINEAR;
    fill.paint.angle = 135;
    fill.paint.count = 2;
    fill.paint.stops[0].color = 0xFF2B5876u;
    fill.paint.stops[1].color = 0xFF4E4376u;
    fill.paint.stops[1].position = DMVSI_PERCENT(100);
    (void)dmvsi_add_fill(doc, &fill);
    memset(&t, 0, sizeof(t));
    t.font = dmvsi_font(doc, INTER, 16, 0, NULL);
    t.x = DMVSI_PX(12) + 20;            /* 12.3 px: snapped to 12 */
    t.baseline = DMVSI_PX(30);
    t.text = text;
    t.length = strlen(text);
    t.paint.color = 0xFFFFFFFFu;
    (void)dmvsi_add_text(doc, &t);
    rect(&fill.rect, 200, 10, 10, 10);  /* Off the screen */
    (void)dmvsi_add_fill(doc, &fill);
    (void)dmvsi_end_group(doc);

    memset(&group, 0, sizeof(group));
    group.opacity = 128;
    (void)dmvsi_begin_group(doc, &group);
    memset(&frame, 0, sizeof(frame));
    rect(&frame.rect, 20, 50, 30, 20);
    frame.width = DMVSI_PX(1);
    frame.paint.color = 0x33FFFFFFu;
    (void)dmvsi_add_frame(doc, &frame);
    (void)dmvsi_end_group(doc);
    return doc;
}

DMOD_TEST_STEP(libtodmvs_writes_a_view_that_assembles)
{
    libtodmvs_result_t r;
    dmvsi_doc_t doc = make_doc("Hi \"dmod\"");
    (void)Dmod_FileRemove(TEST_FILE("Inter-Regular.otf.ini"));
    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, TEST_FILE("demo.dmvs"), NULL, &r), 0);
    dmvsi_free(doc);

    const char* v = read_file(TEST_FILE("demo.dmvs"));
    DMOD_TEST_EXPECT_TRUE(has(v, ".view   demo_view\n.size   100, 80\n.entry  draw\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, ".font   inter_regular_16, \"inter-regular-16\"\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, ", LINEAR, 135, #2B5876 0, #4E4376 100\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "draw:\n        FILL    #102030\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        BOX     @panel, 10, 10, 50, 30\n        RRECT   0, 0, 50, 30, 4, g"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        TEXT    2, 4, 89, 20, \"Hi \\\"dmod\\\"\", inter_regular_16, #FFFFFF, LEFT|TOP\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        OPACITY 128\n        FRAME   0, 0, 30, 20, 1, #33FFFFFF\n        END\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        RET\n"));
    DMOD_TEST_EXPECT_EQ(r.boxes, 2u);
    DMOD_TEST_EXPECT_EQ(r.skipped, 1u);         /* the shape off the screen */
    DMOD_TEST_EXPECT_EQ(r.fonts, 1u);
    DMOD_TEST_EXPECT_TRUE(r.gradients >= 2u);   /* the panel's, the shadow's */
    DMOD_TEST_EXPECT_TRUE(assembles(TEST_FILE("demo.dmvs"), TEST_FILE("demo.dmv")));

    /* The font next to it, with its .ini for dmod */
    DMOD_TEST_EXPECT_TRUE(Dmod_FileAvailable(TEST_FILE("Inter-Regular.otf")));
    const char* ini = read_file(TEST_FILE("Inter-Regular.otf.ini"));
    DMOD_TEST_EXPECT_TRUE(has(ini, "[inter-regular-16]\nsize     = 16\nchars    = 0x20,0x22,0x48,0x64,0x69,0x6D,0x6F\n"));
}

DMOD_TEST_STEP(libtodmvs_adds_to_the_fonts_of_other_views)
{
    dmvsi_doc_t doc = make_doc("Hi \"dmod\"");
    (void)Dmod_FileRemove(TEST_FILE("Inter-Regular.otf.ini"));
    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, TEST_FILE("first.dmvs"), NULL, NULL), 0);
    dmvsi_free(doc);

    doc = make_doc("Odd 0123");
    dmvsi_text_t t;
    memset(&t, 0, sizeof(t));
    t.font = dmvsi_font(doc, INTER, 12, -55, NULL);
    t.text = "ok";
    t.length = 2;
    t.baseline = DMVSI_PX(70);
    t.paint.color = 0xFF000000u;
    DMOD_TEST_EXPECT_EQ(dmvsi_add_text(doc, &t), 0);
    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, TEST_FILE("other.dmvs"), NULL, NULL), 0);
    dmvsi_free(doc);

    const char* v = read_file(TEST_FILE("other.dmvs"));
    DMOD_TEST_EXPECT_TRUE(has(v, ".font   inter_regular_12_t_0_55, \"inter-regular-12-t-0.55\"\n"));
    DMOD_TEST_EXPECT_TRUE(assembles(TEST_FILE("other.dmvs"), TEST_FILE("other.dmv")));
    const char* ini = read_file(TEST_FILE("Inter-Regular.otf.ini"));
    DMOD_TEST_EXPECT_TRUE(has(ini, "[inter-regular-16]\nsize     = 16\nchars    = 0x20,0x22,0x30-0x33,0x48,0x4F,0x64,0x69,0x6D,0x6F\n"));
    DMOD_TEST_EXPECT_TRUE(has(ini, "[inter-regular-12-t-0.55]\nsize     = 12\ntracking = -0.55\nchars    = 0x6B,0x6F\n"));
}

DMOD_TEST_STEP(libtodmvs_paints_shadows_around_what_casts_them)
{
    dmvsi_doc_t doc = dmvsi_new();
    dmvsi_shadow_t s;
    (void)dmvsi_set_view(doc, "shadows", 200, 200);
    memset(&s, 0, sizeof(s));

    /* A blurred circle: one CIRCLE */
    rect(&s.shape, 50, 50, 48, 48);
    s.radius = DMVSI_PX(24);
    s.sigma = DMVSI_PX(24);
    s.color = 0x33FACC15u;
    DMOD_TEST_EXPECT_EQ(dmvsi_add_shadow(doc, &s), 0);
    /* An inset shadow along the screen's edges */
    rect(&s.shape, 0, 0, 200, 200);
    rect(&s.hole, 0, 0, 200, 200);
    s.radius = 0;
    s.sigma = DMVSI_PX(5);
    s.color = 0x80000000u;
    s.flags = DMVSI_SHADOW_INSET;
    DMOD_TEST_EXPECT_EQ(dmvsi_add_shadow(doc, &s), 0);
    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, TEST_FILE("shadows.dmvs"), NULL, NULL), 0);
    dmvsi_free(doc);

    const char* v = read_file(TEST_FILE("shadows.dmvs"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        CIRCLE  74, 74, 96, g1\n"));
    /* In the middle 1 - exp(-R^2 / 2 sigma^2) = 0.39 of 0x33 */
    DMOD_TEST_EXPECT_TRUE(has(v, ".gradient g1, RADIAL, 50, 50, 50, 50, #14FACC15 0, "));
    DMOD_TEST_EXPECT_TRUE(has(v, "        RECT    0, 0, 200, 15, g2\n"));          /* the inset's top strip */
    DMOD_TEST_EXPECT_TRUE(has(v, ".gradient g2, LINEAR, 180, #40000000 0, "));     /* half at the edge */
    DMOD_TEST_EXPECT_TRUE(assembles(TEST_FILE("shadows.dmvs"), TEST_FILE("shadows.dmv")));
}

DMOD_TEST_STEP(libtodmvs_reports_what_it_cannot_write)
{
    dmvsi_doc_t doc = dmvsi_new();
    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, TEST_FILE("empty.dmvs"), NULL, NULL), -EINVAL);   /* no view */
    DMOD_TEST_EXPECT_EQ(dmvsi_set_view(doc, "v", 10, 10), 0);
    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, LIBTODMVS_TEST_DIR "/no/such/dir/v.dmvs", NULL, NULL), -EIO);
    dmvsi_free(doc);
    DMOD_TEST_EXPECT_EQ(libtodmvs_convert_file(TEST_FILE("missing.html"), TEST_FILE("x.dmvs"), NULL, NULL, NULL), -ENOENT);
}
