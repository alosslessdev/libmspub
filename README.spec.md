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
| `MSPUBFeed.h/.cpp` | translates the model into `MSPUBCollector` calls (pages, z-ordered shapes and groups, geometry/rotation, text, images, palette) |

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

## Wiring into the importer

`MSPUBFeed.cpp` now drives `MSPUBCollector` from the model:

* **Dispatch** — `MSPUBDocument::parse` builds the model for every `0x22`
  document and uses the feed when there is no `Quill` stream (Publisher
  95/97) or when `Contents[0x0C] == 616` (Publisher 98).  Documents with a
  Quill stream and any other header tag keep the 2000+ parser, and the
  `0x2C` path is untouched.  Both `0xACE8` and `0xACE7` are accepted.
* **Structure** — pages in document order with their z-order child lists;
  groups via `beginGroup`/`setCurrentGroupSeqNum`/`endGroup`; shapes with
  `setShapeType`, `setShapeCoordinatesInEmu`, `setShapeRotation` (the model's
  counter-clockwise tenths converted into the collector's screen-positive
  angle), `setShapePage` and `setShapeOrder`.
* **Text** — Publisher 95/97: the CP1252 character array split into `0x0C`
  frame blocks and `0x0F` table cells; the block ordinal is assigned to the
  text-bearing `0x0000`/`0x0001` shapes ordered by their `u16@+0x46` text id
  (the only shape-side link the format carries, chapter 13).  Publisher 98:
  the Quill `TEXT` array (UTF-8 from the model, converted to UTF-16LE for the
  collector), attached to the `0x0008` text frames.
* **Images** — each `0x0021` payload is passed to `addImage(..., WMF, ...)`
  and bound to its owner (`0x0002`/`0x0003`, or the shape that owns a `0x0027`
  picture object).
* **Palette** — the eight `0x0047` references are resolved through the colour
  decoder (56-entry static table, literal RGB, automatic) and added with
  `addPaletteColor`.
* **Fill and line** — for the Publisher 95/97 record layout the model exposes
  the `+0x23` pen and `+0x2E` fill references and the `+0x22` width code;
  the feed emits `addShapeLine`/`setShapeFill`.

`docs/INTEGRATION.md` expands these with the exact offsets and the conditions
under which each applies.

### Remaining gaps

* The collector writes a page only when it has at least one shape, so empty
  pages are still omitted from the drawing.
* Publisher 98's Quill stream does not persist a text-object → shape link
  (chapter 13 §13.16); when the block and frame counts differ the whole `TEXT`
  array is attached to the first `0x0008` frame.
* Publisher 98 shapes are emitted with geometry only; their shifted fill/line
  fields (`B + 8`) are not decoded.
* Table cells are emitted as paragraphs; the `0x0001` row/column tail is not
  yet turned into `TableInfo`.
* Polygon outlines (the `0x0002`/`0x0003` vertex tail), flips and embedded
  objects are not emitted.
* Page margins are not part of the 95/97/98 record set, so only the document
  page size is handed to the collector.

## License

MPL-2.0, unchanged from upstream. The new files carry the same header.
