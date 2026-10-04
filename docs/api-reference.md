# todmvs API Reference

## libtodmvs

`#include "libtodmvs.h"`

| Function | |
|----------|-|
| `int libtodmvs_write(doc, output, options, result)` | Write the view of a dmvsi document into `output` (.dmvs), its fonts and images next to it |
| `int libtodmvs_convert_file(input, output, convert, options, result)` | Convert a file with its dmvsi converter (`convert`: root element, screen size, resource maps - see dmvsi) and write its view |

Both return 0, `-EINVAL`, `-EIO` (cannot write), `-ENOENT` (a font or an
image is missing), `-ENOMEM`, or what the conversion failed with. The view is
written to a temporary file and renamed: a failed conversion leaves no
half-written view.

`libtodmvs_options_t`:

| Field | |
|-------|-|
| `source` | Named in the view's header comment |
| `no_assets` | Write only the `.dmvs` - no fonts, no images |

`libtodmvs_result_t`: `boxes`, `instructions` (drawing), `gradients`,
`fonts` (declared), `skipped` (shapes outside the screen or their box, or
invisible).

## todmvs

```
todmvs [options] PAGE
```

| Option | |
|--------|-|
| `-o OUTPUT` | The `.dmvs` (default: PAGE with `.dmvs` in place of its extension) |
| `-r ID` | The element the view is made of (default: the whole page) |
| `-s WIDTHxHEIGHT` | The screen the page is laid out for |
| `-n NAME` | The view's name |
| `-m FROM=TO` | A resource is elsewhere - split at the last `=` (a URL has them in its query); repeatable, up to 16 |
| `--no-assets` | Write only the view |
| `-q` | Print nothing but errors |

Exit code 0 on success, 1 on an error (printed).
