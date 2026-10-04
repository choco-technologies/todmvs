#ifndef LIBTODMVS_H
#define LIBTODMVS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod.h"
#include "dmvsi.h"
#include "libtodmvs_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libtodmvs - views (.dmvs) of documents that dmvsi converters make of
 * pages (HTML, ...). A document is groups and shapes at absolute
 * positions; the view is dmview assembly:
 *
 *  - a group becomes a BOX only when it has to - it clips, fades
 *    (OPACITY) or scrolls; the others are flattened into their box,
 *  - what is outside the screen or the box it is in is left out,
 *  - rectangles, outlines, text and images are RECT / RRECT / FILL,
 *    FRAME / RFRAME, TEXT, IMAGE / ICON; colors and gradients are
 *    declared once (.gradient),
 *  - shadows and blurs, which dmview does not draw, are painted with
 *    gradients around the shape: their falloff as stops,
 *  - every font is a .font of the size and letter spacing the text uses.
 *
 * Next to the view, the fonts are written as todmvf (and dmod's
 * DMOD_ASSETS_PATHS) makes them: a copy of each font file and its .ini
 * with a section for every size, with only the characters the text uses -
 * an .ini that is there already keeps its other sections. The images are
 * copied there too; the view names them as .dmvi, which dmod converts them
 * into. The output directory is ready to be one of DMOD_ASSETS_PATHS.
 */

/** How a view is written. */
typedef struct
{
    const char*     source;         /**< Named in the view's header comment, NULL: none */
    bool            no_assets;      /**< Write only the .dmvs - no fonts, no images */
} libtodmvs_options_t;

/** What was written. */
typedef struct
{
    uint32_t        boxes;          /**< BOX instructions */
    uint32_t        instructions;   /**< Drawing instructions */
    uint32_t        gradients;      /**< .gradient declarations */
    uint32_t        fonts;          /**< .font declarations */
    uint32_t        skipped;        /**< Shapes left out: outside the screen or their box, invisible */
} libtodmvs_result_t;

/**
 * @brief Write the view of a document into @p output (.dmvs), and its fonts
 *        and images next to it.
 * @param options May be NULL
 * @param result May be NULL
 * @return 0, -EINVAL, -EIO (cannot write), -ENOENT (a font or an image file is missing), -ENOMEM
 */
dmod_libtodmvs_api(1.0, int, _write, ( dmvsi_doc_t doc, const char* output, const libtodmvs_options_t* options, libtodmvs_result_t* result ));

/**
 * @brief Convert a file with its dmvsi converter (dmvsi_convert_file()) and
 *        write its view as libtodmvs_write() does.
 * @param convert How the file is converted (root element, screen size, resources), may be NULL
 * @return 0, what dmvsi_convert_file() or libtodmvs_write() failed with
 */
dmod_libtodmvs_api(1.0, int, _convert_file, ( const char* input, const char* output, const dmvsi_options_t* convert,
                                              const libtodmvs_options_t* options, libtodmvs_result_t* result ));

#ifdef __cplusplus
}
#endif

#endif /* LIBTODMVS_H */
