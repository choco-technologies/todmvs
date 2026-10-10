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
    DMOD_TEST_EXPECT_TRUE(has(v, "        CIRCLE  74, 74, 50, g1\n"));        /* as far as it is visible */
    /* In the middle 1 - exp(-R^2 / 2 sigma^2) = 0.39 of 0x33 */
    DMOD_TEST_EXPECT_TRUE(has(v, ".gradient g1, RADIAL, 50, 50, 50, 50, #14FACC15 0, "));
    DMOD_TEST_EXPECT_TRUE(has(v, "        RECT    0, 0, 200, 10, g2\n"));          /* the inset's top strip, as far as visible */
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

DMOD_TEST_STEP(libtodmvs_writes_behaviour)
{
    dmvsi_doc_t doc = dmvsi_new();
    dmvsi_group_t group;
    dmvsi_fill_t fill;
    (void)dmvsi_set_view(doc, "screens", 100, 80);
    dmvsi_var_t y = dmvsi_add_var(doc, "window-y", DMVSI_PX(80));        /* Below the screen */
    dmvsi_var_t alpha = dmvsi_add_var(doc, "home-alpha", 255);
    dmvsi_var_t open = dmvsi_add_var(doc, "open", 0);

    /* A tile that opens the window */
    dmvsi_action_t actions[5];
    memset(actions, 0, sizeof(actions));
    actions[0].kind = DMVSI_ACT_IF_EQ;
    actions[0].var = open;
    actions[1].kind = DMVSI_ACT_ANIMATE;
    actions[1].var = y;
    actions[1].value = 0;
    actions[1].duration = 300;
    actions[1].easing[0] = 250;
    actions[1].easing[1] = 800;
    actions[1].easing[2] = 250;
    actions[1].easing[3] = 1000;
    actions[2].kind = DMVSI_ACT_SET;
    actions[2].var = alpha;
    actions[2].value = 77;
    actions[3].kind = DMVSI_ACT_TOGGLE;
    actions[3].var = open;
    actions[4].kind = DMVSI_ACT_END;
    dmvsi_handler_t h = dmvsi_add_handler(doc, actions, 5);

    memset(&group, 0, sizeof(group));
    group.opacity = 255;
    group.name = "home";
    (void)dmvsi_begin_group(doc, &group);
    (void)dmvsi_bind(doc, DMVSI_BIND_OPACITY, alpha);
    memset(&group, 0, sizeof(group));
    group.opacity = 255;
    group.name = "tile";
    group.rect.x = DMVSI_PX(10);
    group.rect.y = DMVSI_PX(10);
    group.rect.w = DMVSI_PX(20);
    group.rect.h = DMVSI_PX(20);
    (void)dmvsi_begin_group(doc, &group);
    (void)dmvsi_on_click(doc, h);
    memset(&fill, 0, sizeof(fill));
    rect(&fill.rect, 12, 12, 16, 16);
    fill.paint.color = 0xFF3D85F5u;
    (void)dmvsi_add_fill(doc, &fill);
    (void)dmvsi_end_group(doc);
    (void)dmvsi_end_group(doc);

    /* The window, off the screen until it is opened */
    memset(&group, 0, sizeof(group));
    group.opacity = 255;
    group.flags = DMVSI_GROUP_CLIP;
    group.name = "window";
    rect(&group.rect, 0, 80, 100, 80);
    (void)dmvsi_begin_group(doc, &group);
    (void)dmvsi_bind(doc, DMVSI_BIND_Y, y);
    rect(&fill.rect, 0, 80, 100, 80);
    fill.paint.color = 0xFF101010u;
    (void)dmvsi_add_fill(doc, &fill);
    (void)dmvsi_end_group(doc);

    libtodmvs_result_t r;
    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, TEST_FILE("screens.dmvs"), NULL, &r), 0);
    dmvsi_free(doc);
    const char* v = read_file(TEST_FILE("screens.dmvs"));
    DMOD_TEST_EXPECT_TRUE(has(v, ".var    $window_y, int, 80\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, ".var    $window_y_to, int, 0\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, ".timer  16, animate\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        BOX     @home, 10, 10, 20, 20\n        OPACITY $home_alpha\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        BOX     @tile, 0, 0, 20, 20\n        ON      CLICK, h1\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        BOX     @window, 0, $window_y, 100, 80, OPAQUE\n        FILL    #101010\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "h1:\n        JNE     $open, 0, .i1\n        SET     $window_y_from, $window_y\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "        SET     $home_alpha, 77\n        TOGGLE  $open\n.i1:\n        RET\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "ease1:\n"));
    DMOD_TEST_EXPECT_TRUE(assembles(TEST_FILE("screens.dmvs"), TEST_FILE("screens.dmv")));
}

DMOD_TEST_STEP(libtodmvs_writes_the_sizes_of_images)
{
    /* Any file: the writer copies it and names it */
    void* f = Dmod_FileOpen(TEST_FILE("cover.jpg"), "wb");
    DMOD_TEST_EXPECT_TRUE(f != NULL);
    if (f == NULL)
        return;
    Dmod_FileWrite("jpeg", 1, 4, f);
    Dmod_FileClose(f);
    (void)Dmod_FileRemove(TEST_FILE("cover.jpg.ini"));

    dmvsi_doc_t doc = dmvsi_new();
    DMOD_TEST_EXPECT_TRUE(doc != NULL);
    if (doc == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(dmvsi_set_view(doc, "covers", 100, 80), 0);
    dmvsi_image_t im;
    memset(&im, 0, sizeof(im));
    im.path = TEST_FILE("cover.jpg");
    rect(&im.rect, 0, 0, 40, 40);
    DMOD_TEST_EXPECT_EQ(dmvsi_add_image(doc, &im), 0);                  /* At its own size */
    im.width = DMVSI_PX(60);                                            /* Covering 40 x 40, in the middle */
    im.height = DMVSI_PX(40);
    im.flags = DMVSI_IMAGE_CENTER | DMVSI_IMAGE_MIDDLE;
    DMOD_TEST_EXPECT_EQ(dmvsi_add_image(doc, &im), 0);
    im.blur = DMVSI_PX(8);                                              /* ... and blurred */
    im.flags = DMVSI_IMAGE_RIGHT | DMVSI_IMAGE_BOTTOM;
    DMOD_TEST_EXPECT_EQ(dmvsi_add_image(doc, &im), 0);
    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, TEST_FILE("covers.dmvs"), NULL, NULL), 0);
    dmvsi_free(doc);

    const char* v = read_file(TEST_FILE("covers.dmvs"));
    DMOD_TEST_EXPECT_TRUE(has(v, "IMAGE   0, 0, 40, 40, \"cover.dmvi\", LEFT|TOP\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "IMAGE   0, 0, 40, 40, \"cover-60x40.dmvi\", CENTER|MIDDLE\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "IMAGE   0, 0, 40, 40, \"cover-60x40-b8.dmvi\", RIGHT|BOTTOM\n"));
    const char* ini = read_file(TEST_FILE("cover.jpg.ini"));
    DMOD_TEST_EXPECT_TRUE(has(ini, "\n[cover]\n\n[cover-60x40]\nsize = 60x40\n\n[cover-60x40-b8]\nsize = 60x40\nblur = 8\n"));
    DMOD_TEST_EXPECT_TRUE(assembles(TEST_FILE("covers.dmvs"), TEST_FILE("covers.dmv")));
}

static dmvsi_action_t act(uint8_t kind, dmvsi_var_t var, dmvsi_var_t operand, int32_t value, const char* text)
{
    dmvsi_action_t a;
    memset(&a, 0, sizeof(a));
    a.kind = kind;
    a.var = var;
    a.operand = operand;
    a.value = value;
    a.text = text;
    return a;
}

DMOD_TEST_STEP(libtodmvs_writes_code)
{
    /* A speedometer: every 35 ms 2 km/h more up to 68 - its text and a bar as wide as the speed */
    (void)Dmod_FileRemove(TEST_FILE("Inter-Regular.otf.ini"));
    dmvsi_doc_t doc = dmvsi_new();
    (void)dmvsi_set_view(doc, "speed", 100, 40);
    dmvsi_var_t speed = dmvsi_add_var(doc, "speed", 0);
    dmvsi_var_t bar = dmvsi_add_var(doc, "bar", 0);
    dmvsi_var_t label = dmvsi_add_text_var(doc, "label", 12, "0 km/h");

    /* show: label = speed + " km/h"; bar = speed */
    dmvsi_handler_t show = dmvsi_new_handler(doc);
    dmvsi_action_t tick[8];
    tick[0] = act(DMVSI_ACT_ADD, speed, 0, 2, NULL);
    tick[1] = act(DMVSI_ACT_IF_GE, speed, 0, 68, NULL);
    tick[2] = act(DMVSI_ACT_SET, speed, 0, 68, NULL);
    tick[3] = act(DMVSI_ACT_ELSE, 0, 0, 0, NULL);
    tick[4] = act(DMVSI_ACT_MIN, speed, 0, 68, NULL);
    tick[5] = act(DMVSI_ACT_END, 0, 0, 0, NULL);
    tick[6] = act(DMVSI_ACT_CALL, 0, 0, 0, NULL);
    tick[6].handler = show;                                 /* Made after it */
    tick[7] = act(DMVSI_ACT_RETURN, 0, 0, 0, NULL);
    dmvsi_handler_t step = dmvsi_add_handler(doc, tick, 8);
    dmvsi_action_t text[4];
    text[0] = act(DMVSI_ACT_FORMAT, label, speed, 0, "%d");
    dmvsi_var_t since = dmvsi_add_var(doc, "since", 0);
    dmvsi_action_t now = act(DMVSI_ACT_SET, since, DMVSI_VAR_TIME, 0, NULL);
    DMOD_TEST_EXPECT_TRUE(dmvsi_add_handler(doc, &now, 1) != 0);
    text[1] = act(DMVSI_ACT_APPEND, label, 0, 0, " km/h");
    text[2] = act(DMVSI_ACT_SET, bar, speed, 0, NULL);
    text[3] = act(DMVSI_ACT_LOOP, 0, 0, 0, NULL);
    dmvsi_action_t more[3];
    more[0] = act(DMVSI_ACT_SUB, bar, 0, 1, NULL);
    more[1] = act(DMVSI_ACT_BREAK, 0, 0, 0, NULL);
    more[2] = act(DMVSI_ACT_END, 0, 0, 0, NULL);
    dmvsi_action_t all[7] = { text[0], text[1], text[2], text[3], more[0], more[1], more[2] };
    DMOD_TEST_EXPECT_EQ(dmvsi_set_handler(doc, show, all, 7), 0);
    DMOD_TEST_EXPECT_EQ(dmvsi_add_timer(doc, 35, step), 0);
    DMOD_TEST_EXPECT_EQ(dmvsi_set_init(doc, show), 0);

    dmvsi_group_t group;
    memset(&group, 0, sizeof(group));
    group.opacity = 255;
    group.name = "bar";
    rect(&group.rect, 0, 30, 1, 10);
    group.flags = DMVSI_GROUP_CLIP;
    (void)dmvsi_begin_group(doc, &group);
    DMOD_TEST_EXPECT_EQ(dmvsi_bind(doc, DMVSI_BIND_W, bar), 0);
    dmvsi_fill_t fill;
    memset(&fill, 0, sizeof(fill));
    rect(&fill.rect, 0, 30, 100, 10);
    fill.paint.color = 0xFF3B82F6u;
    (void)dmvsi_add_fill(doc, &fill);
    (void)dmvsi_end_group(doc);

    dmvsi_text_t line;
    memset(&line, 0, sizeof(line));
    line.x = DMVSI_PX(10);
    line.baseline = DMVSI_PX(20);
    line.text = "0 km/h";
    line.length = 6;
    line.font = dmvsi_font(doc, INTER, 16, 0, NULL);
    line.paint.color = 0xFFFFFFFFu;
    line.var = label;
    line.chars = "0123456789 km/h";
    line.width = DMVSI_PX(80);
    line.align = DMVSI_TEXT_CENTER;
    DMOD_TEST_EXPECT_EQ(dmvsi_add_text(doc, &line), 0);

    DMOD_TEST_EXPECT_EQ(libtodmvs_write(doc, TEST_FILE("speed.dmvs"), NULL, NULL), 0);
    dmvsi_free(doc);

    const char* v = read_file(TEST_FILE("speed.dmvs"));
    DMOD_TEST_EXPECT_TRUE(has(v, ".var    $label, str[12], \"0 km/h\"\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, ".timer  35, h2\n.init   h1\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "h3:\n        SET     $since, $time\n"));               /* show is h1: made first */
    DMOD_TEST_EXPECT_TRUE(has(v, "BOX     @bar, 0, 30, $bar, 10, OPAQUE\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, ", $label, inter_regular_16, #FFFFFF, CENTER|TOP\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "h2:\n        ADD     $speed, 2\n        JLT     $speed, 68, .i1\n        SET     $speed, 68\n"
                                 "        JMP     .f1\n.i1:\n        MIN     $speed, 68\n.f1:\n        CALL    h1\n        RET\n        RET\n"));
    DMOD_TEST_EXPECT_TRUE(has(v, "h1:\n        FORMAT  $label, \"%d\", $speed\n        APPEND  $label, \" km/h\"\n"
                                 "        SET     $bar, $speed\n.l1:\n        SUB     $bar, 1\n        JMP     .b1\n        JMP     .l1\n.b1:\n"));
    DMOD_TEST_EXPECT_TRUE(assembles(TEST_FILE("speed.dmvs"), TEST_FILE("speed.dmv")));

    /* Its font has every character the label may show */
    const char* ini = read_file(TEST_FILE("Inter-Regular.otf.ini"));
    bool every = has(ini, "chars    = 0x20,0x2F-0x39,0x68,0x6B,0x6D\n");
    if (!every)
        Dmod_Printf("    %s\n", ini);
    DMOD_TEST_EXPECT_TRUE(every);
}
