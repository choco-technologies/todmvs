#include "dmod.h"
#include "libtodmvs.h"
#include <errno.h>
#include <string.h>

/**
 * @brief todmvs - convert a page (HTML, ... - whatever the dmvsi converters
 *        know) into a dmview view (.dmvs), with its fonts and images next
 *        to it. See libtodmvs.
 */

#define PATH_MAX_LEN    256
#define MAX_MAPS        16

static void print_usage(const char* name)
{
    Dmod_Printf("Usage: %s [options] PAGE\n", name);
    Dmod_Printf("  -o OUTPUT        the .dmvs file (default: PAGE with .dmvs in place of its extension)\n");
    Dmod_Printf("  -r ID            the element the view is made of (default: the whole page)\n");
    Dmod_Printf("  -s WIDTHxHEIGHT  the screen the page is laid out for (default: the converter's)\n");
    Dmod_Printf("  -n NAME          the view's name (default: the page's file name)\n");
    Dmod_Printf("  -m FROM=TO       a resource is elsewhere: a URL, or its beginning ending in '/', and the\n");
    Dmod_Printf("                   file, or directory, it is (style sheets, fonts, images); repeatable\n");
    Dmod_Printf("  --no-assets      write only the view - no fonts, no images next to it\n");
    Dmod_Printf("  -q               print nothing but errors\n");
}

/* PAGE.html -> PAGE.dmvs */
static void default_output(const char* input, char* output, size_t size)
{
    const char* dot = strrchr(input, '.');
    const char* slash = strrchr(input, '/');
    size_t len = (dot != NULL && (slash == NULL || dot > slash)) ? (size_t)(dot - input) : strlen(input);
    if (len + sizeof(".dmvs") > size)
        len = size - sizeof(".dmvs");
    memcpy(output, input, len);
    memcpy(output + len, ".dmvs", sizeof(".dmvs"));
}

/* "480x272" */
static bool parse_size(const char* s, uint16_t* w, uint16_t* h)
{
    uint32_t v[2] = { 0, 0 };
    int part = 0;
    for (; *s != '\0'; s++)
    {
        if (*s == 'x' && part == 0)
            part = 1;
        else if (*s >= '0' && *s <= '9' && v[part] <= 0xFFFFu)
            v[part] = v[part] * 10U + (uint32_t)(*s - '0');
        else
            return false;
    }
    if (part == 0 || v[0] == 0 || v[1] == 0 || v[0] > 0xFFFFu || v[1] > 0xFFFFu)
        return false;
    *w = (uint16_t)v[0];
    *h = (uint16_t)v[1];
    return true;
}

/* "FROM=TO" at its last '=' (a URL has them in its query) */
static bool parse_map(char* s, dmvsi_map_t* map)
{
    char* eq = strrchr(s, '=');
    if (eq == NULL || eq == s || eq[1] == '\0')
        return false;
    *eq = '\0';
    map->from = s;
    map->to = eq + 1;
    return true;
}

static const char* error_text(int ret)
{
    switch (ret)
    {
        case -ENOENT:  return "cannot read the page, or a font or image it uses";
        case -ENOTSUP: return "not supported (no converter for it)";
        case -EBADMSG: return "the page is damaged, or describes no view";
        case -ENOMEM:  return "out of memory";
        case -EINVAL:  return "invalid arguments";
        default:       return "cannot write the output";
    }
}

int main(int argc, char* argv[])
{
    const char* input = NULL;
    const char* output = NULL;
    char default_path[PATH_MAX_LEN];
    dmvsi_map_t maps[MAX_MAPS];
    dmvsi_options_t convert;
    libtodmvs_options_t options;
    libtodmvs_result_t result;
    bool quiet = false;

    memset(&convert, 0, sizeof(convert));
    memset(&options, 0, sizeof(options));
    memset(&result, 0, sizeof(result));
    convert.maps = maps;

    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        bool has_value = i + 1 < argc;
        if (strcmp(a, "-o") == 0 && has_value)
            output = argv[++i];
        else if (strcmp(a, "-r") == 0 && has_value)
            convert.root = argv[++i];
        else if (strcmp(a, "-n") == 0 && has_value)
            convert.name = argv[++i];
        else if (strcmp(a, "-s") == 0 && has_value)
        {
            if (!parse_size(argv[++i], &convert.width, &convert.height))
            {
                Dmod_Printf("todmvs: invalid size '%s' (WIDTHxHEIGHT)\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(a, "-m") == 0 && has_value)
        {
            if (convert.map_count >= MAX_MAPS || !parse_map(argv[++i], &maps[convert.map_count]))
            {
                Dmod_Printf("todmvs: invalid resource map '%s' (FROM=TO, at most %d)\n", argv[i], MAX_MAPS);
                return 1;
            }
            convert.map_count++;
        }
        else if (strcmp(a, "--no-assets") == 0)
            options.no_assets = true;
        else if (strcmp(a, "-q") == 0)
            quiet = true;
        else if (a[0] != '-' && input == NULL)
            input = a;
        else
        {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (input == NULL)
    {
        print_usage(argv[0]);
        return 1;
    }
    if (output == NULL)
    {
        default_output(input, default_path, sizeof(default_path));
        output = default_path;
    }

    int ret = libtodmvs_convert_file(input, output, &convert, &options, &result);
    if (ret != 0)
    {
        Dmod_Printf("todmvs: %s: %s (%d)\n", input, error_text(ret), ret);
        return 1;
    }
    if (!quiet)
    {
        Dmod_Printf("todmvs: %s -> %s: %u boxes, %u instructions, %u gradients, %u fonts\n", input, output,
                    (unsigned)result.boxes, (unsigned)result.instructions, (unsigned)result.gradients, (unsigned)result.fonts);
    }
    return 0;
}
