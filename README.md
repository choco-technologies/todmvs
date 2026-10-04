# todmvs

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/todmvs/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/todmvs/actions/workflows/ci.yml)

Pages into dmview views (`.dmvs`): a design made in HTML and CSS - or any
format a [dmvsi](https://github.com/choco-technologies/dmvsi) converter knows
- becomes a view, with its fonts and images next to it.

```
page.html ──(dmvsi: dmvs_html)──► document ──(libtodmvs)──► page.dmvs + fonts + images ──(dmod)──► .dmv, .dmvf, .dmvi
```

| Module | Kind | Role |
|--------|------|------|
| `libtodmvs` | Library | Writes the view of a dmvsi document; converts a file (dmvsi + writing) |
| `todmvs` | Application | The command-line tool on top of `libtodmvs` |

Both run on a PC (at build time) and on a device.

## The view

- A group becomes a `BOX` only when it clips, fades (`OPACITY`) or scrolls
  (`SCROLL`); the others are flattened into their box. `OPAQUE` when the
  box's first shape covers it with opaque pixels.
- What is outside the screen (or its box) is left out - e.g. the windows
  of a page that slide in with JavaScript.
- Colors are written in place, gradients declared once each.
- **Shadows and blurs**, which dmview does not draw, are painted with
  gradients: the blur's falloff as their stops, on pieces around the shape
  (bands straight out, radial corners), only outside the element casting it
  and only as far as it is visible; a blurred circle is one `CIRCLE` with
  the exact falloff of a blurred disk.
- Text is `TEXT` on its baseline, in a `.font` of the size and the letter
  spacing it is drawn in.
- **Behaviour**: the document's variables are `.var`s; a group bound to one
  is a `BOX` at `$x` / `$y` or with `OPACITY $a` (kept while it is off the
  screen - a window that slides in), a clicked one has `ON CLICK`; a handler
  is a label of `SET`, `TOGGLE` and jumps. An animated variable moves each
  frame (`.timer 16`) along its CSS easing - a `cubic-bezier` made 16 lines.

## Fonts and images

Next to the view the writer puts what dmod's `DMOD_ASSETS_PATHS` makes the
rest of from: a copy of every font file the view draws with, and its `.ini`
with a section for each size and letter spacing - with only the characters
the view's text uses (a clock in 48 px is just its digits and the colon, not a
whole font). An `.ini` that is there already keeps its other sections, so
several views can share a directory. Images are copied too, and named in
the view as the `.dmvi` dmod converts them into.

The output directory is ready to be one of `DMOD_ASSETS_PATHS`.

## Usage

```
todmvs [options] PAGE
  -o OUTPUT        the .dmvs file (default: PAGE with .dmvs in place of its extension)
  -r ID            the element the view is made of (default: the whole page)
  -s WIDTHxHEIGHT  the screen the page is laid out for (default: the converter's)
  -n NAME          the view's name (default: the page's file name)
  -m FROM=TO       a resource is elsewhere: a URL, or its beginning ending in '/', and the
                   file, or directory, it is (style sheets, fonts, images); repeatable
  --no-assets      write only the view - no fonts, no images next to it
  -q               print nothing but errors
```

A page made with CDNs - Tailwind's Play CDN, Font Awesome, Google Fonts:

```bash
todmvs ui.html -r screen -o views/ui.dmvs \
    -m https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/=fontawesome/ \
    -m https://fonts.googleapis.com/css2=fonts/inter.css
```

(`fonts/inter.css`: the `@font-face` rules of the font files you have.)
See [docs/api-reference.md](docs/api-reference.md) for the library.

## Building

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout.

## Testing

The tests write documents made in code and assemble the views with
libtodmv:

```bash
cd build
ctest --output-on-failure
```

## License

MIT - see [LICENSE](LICENSE).
