# libmspub + a from-specification implementation of the Publisher 95/97/98 format

This is a fork of [LibreOffice/libmspub](https://github.com/LibreOffice/libmspub)
(MPL-2.0, read-only mirror) that adds a **dependency-free implementation of the
file-format specification** published at
[alosslessdev/mspub-file-format](https://github.com/alosslessdev/mspub-file-format).

The upstream importer handles a subset of Publisher 95/97/98. The new code
implements the layers upstream does not: the corrected chunk directory, the
Publisher 98 record shifts and page slots, the Publisher 95/97 in-`Contents`
text store (including table cells), the Quill text stream, palettes, and
embedded-object references.

## What is implemented

`src/lib/spec/` — self-contained C++17, no external dependencies:

| File | Contents |
|---|---|
| `MSPUBSpec.h` | document model: chunks, pages, shapes, stories, images, palettes, object references, cell blocks |
| `MSPUBSpec.cpp` | CFB v3 reader (FAT, mini-FAT, mini stream), `Contents` header, chunk directory, per-type records, text store, Quill, colour references, JSON |
| `MSPUBExplore.cpp` | `pubexplore` CLI: dump a document as JSON, optionally carve every embedded WMF |

Implemented per spec chapter:

* **01** CFB v3 container: header, DIFAT/FAT, directory tree, mini-FAT and mini
  stream (needed for documents under 4 KiB, e.g. the 2 780-byte `MSPUB.PUB`).
* **02** `Contents` header, including the per-build `header_tag` (125 / 136 / 616)
  and the text-section offset.
* **03** chunk directory: `u16 count`, `u16 idHighWaterMark`, 10-byte records
  `{id, parent, offset, flags}`, and chunk extents derived from the next offset.
* **04/05** document record (EMU page size, coordinate system); PAGE records in
  the 95/97 layout *and* the 98 layout (+4 shift, descriptor `0x1E`), including
  page slots of type `0x41`.
* **06/07** shape and group geometry: rotation in 1/10 degree, signed EMU
  bounding box, group child lists in both layouts.
* **08** `IMAGE_DATA`: length-prefixed WMF, carved out as-is.
* **09** PALETTE (`0x0047`): UTF-16LE scheme name and eight colour references,
  plus the colour-reference decoder (palette index, literal RGB, automatic).
* **10/13** Publisher 95/97 text store: character array, story records
  (`0x001D`, `size == 0x1A + 6n`), and table cells (`0x0C` blocks split on
  `0x0F`).
* **13** Quill stream (`Quill/QuillSub/CONTENTS`): chunk-reference chain and
  UTF-16LE `TEXT`.
* **15** embedded objects: `Objects/Object N` storages and the `0x0022`
  reference records (`"MO"` + ordinal).

## Verification

* **493 / 493** documents from the four media parse without error.
* On 10 samples spanning all three versions the chunk inventory, page count and
  z-order lists, shape count, and image count are identical to the reference
  reader used to produce the specification's expected-output files
  (`tests/expect/*.json` in the documentation repository).
* The Kaitai grammar in the documentation repository is an independent second
  encoding of the same structures and agrees on the same six samples.

No sample documents are redistributed here; the specification repository lists
each file's Internet Archive item, path and SHA-256.

## Building

The new code has no dependencies and builds with any C++17 compiler:

```bash
g++ -std=c++17 -O2 -o pubexplore src/lib/spec/MSPUBSpec.cpp src/lib/spec/MSPUBExplore.cpp
./pubexplore document.pub            # JSON summary
./pubexplore --dump-images out/ document.pub
```

The rest of the repository is upstream libmspub and still builds with its
autotools setup against librevenge, boost and ICU.

## Not yet wired

The model is not yet pushed through `MSPUBCollector` into
`librevenge::RVNGDrawingInterface`. The integration points are:

1. `MSPUBDocument::parse` — route `MSPUB_2K` documents (and the `0xACE7` legacy
   magic) through the new model when a `.pub` has no `Quill` stream, which is
   the case upstream cannot currently import.
2. `MSPUBCollector::addTextShape` / `addText` — feed the text store's character
   array plus story records (`bounds[]`, `paraIds[]`), and split cells on
   `0x0C`/`0x0F` for tables.
3. `MSPUBCollector::addPaletteColor` — feed the eight scheme references from
   `0x0047`, resolved through the colour-reference decoder.
4. `MSPUBParser2k` — apply the Publisher 98 record shifts (+8 shape, +4 page,
   `0x41` page slots) instead of treating every `0x22` file as 97.

`docs/INTEGRATION.md` expands these with the exact offsets and the conditions
under which each applies.

## License

MPL-2.0, unchanged from upstream. The new files carry the same header.
