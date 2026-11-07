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
| `MSPUBSpec.h` | document model: chunks, pages, shapes (with freeform vertices and table geometry), stories, images, palettes, object references, cell blocks |
| `MSPUBSpec.cpp` | CFB v3 reader (FAT, mini-FAT, mini stream), `Contents` header, chunk directory, per-type records, text store, Quill, colour references, JSON |
| `MSPUBExplore.cpp` | `pubexplore` CLI: dump a document as JSON, optionally carve every embedded WMF |
| `MSPUBFeed.h/.cpp` | translates the model into `MSPUBCollector` calls (pages, z-ordered shapes and groups, geometry/rotation, polygons, tables, text, images, palette) |

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
  bounding box, group child lists in both layouts; the draw-time field map
  (flag word `+0x16`, pen `+0x21`…`+0x27`, frame descriptor) with the
  Publisher 98 `+8` insertion; the `0x0003` vertex array (count at `+0x74`,
  stride word `+0x78`, `{i32 x, i32 y}` from `+0x7E`, box-local EMU) and the
  `0x0001` `A`/`B` counts at `+0x4A`/`+0x50` with their 14-byte records.
* **08** `IMAGE_DATA`: length-prefixed WMF, carved out as-is.
* **09** PALETTE (`0x0047`): UTF-16LE scheme name and eight colour references,
  plus the colour-reference decoder (palette index, literal RGB, automatic).
* **10/13** Publisher 95/97 text store: character array, story records
  (`0x001D`, `size == 0x1A + 6n`), and table cells (`0x0C` blocks split on
  `0x0F`, dropping the trailing paragraph-break field that is not a cell).
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

* **Dispatch** — `MSPUBDocument::parse` reads the whole OLE2 file once and
  builds the model for every `0x22` document, then uses the feed when there is
  no `Quill` stream (Publisher 95/97) or when `Contents[0x0C] == 616`
  (Publisher 98).  Documents with a Quill stream and any other header tag keep
  the 2000+ parser, and the `0x2C` path is untouched.  Both `0xACE8` and
  `0xACE7` are accepted.
* **Structure** — pages in document order with their z-order child lists;
  every page of the model is declared with `setPageHasContent`, so pages
  without shapes still produce a `startPage`/`endPage` pair; groups via
  `beginGroup`/`setCurrentGroupSeqNum`/`endGroup`; shapes with `setShapeType`,
  `setShapeCoordinatesInEmu`, `setShapeRotation` (the model's
  counter-clockwise tenths converted into the collector's screen-positive
  angle), `setShapePage` and `setShapeOrder`.  A `0x0002`/`0x0003` freeform
  outline (count pair at `+0x74`, element size at `+0x78`, box-local EMU
  vertices from `+0x7E`) is emitted as a `DynamicCustomShape` (box-local EMU
  vertices, `NOT_PRIMITIVE`), which makes the collector draw the polygon
  instead of its bounding rectangle.  Flag word bit 0 (`+0x16`) is emitted
  as an offset shadow (fixed `(57150, 57150)` EMU down-right, pen colour
  tinted 50 % toward white); no other flag bit is a flip, frame or mirror
  control (chapter 06 §6.7.6), so only the shadow is emitted.
* **Text** — Publisher 95/97: the character array split into `0x0C` frame
  blocks.  The shape side of the link is the `u16@+0x46` text id: every
  shape with text id *i* displays block *i*, and repeated ids share the
  block (one story flowed across several frames).  Content shapes are
  grouped by text id and the *k*-th group owns the *k*-th block; `0xFFFF`
  records and the zero-box gallery defaults own no block (chapter 10
  §10.3.1, chapter 13 §13.14).  A block whose `0x0F` cell count equals the
  `A·B` of a sharing `0x0001` frame is emitted as a real table: one
  paragraph per cell, `setTableCellTextEnds` with the cumulative character
  offsets and `setShapeTableInfo` with the row/column extents from the
  14-byte records.  The format has no table record (chapter 10 §10.1) —
  the grid was always the frame's `A`/`B` counts, so `TableInfo` expresses
  it exactly.  Publisher 98: the Quill `TEXT` array (UTF-8 from the model,
  converted to UTF-16LE for the collector), attached to the `0x0008` text
  frames.


`docs/INTEGRATION.md` expands these with the exact offsets and the conditions
under which each applies.

### Remaining gaps

* **Flips are not emitted.** Chapter 06 §6.7.6 accounts for every bit of the
  shape flag word (`+0x16`) and none of them flips anything: bit 0 is the
  shadow, bits 3/4 the frame pair, bits 8–13 interpretation and group-child
  flags. The only mirror-like data in the family is the per-edge tag byte of
  the geometry quad (the high byte of each `i32` at `+0x06`/`+0x0A`/`+0x0E`/
  `+0x12`), which the field notes leave `[OPEN]`; the specification does not
  define it, so no `setShapeFlip` value can be derived without guessing.
* Publisher 98's *fill* colour is not emitted. The 95/97 slot is `+0x2E`; its
  `+8`-shifted candidate (`+0x36`) holds one constant value in every record of
  the Publisher 98 corpus, which cannot be a per-shape fill, and chapter 06
  §6.9 still lists the inserted run as `[UNKNOWN]`. The line width (`+0x2A`)
  and line colour (`+0x2B`) are read at the shifted offsets the chapter
  names (the shift is 24 bytes for the image-bearing `0x0002`/`0x0003`).
* Publisher 98's Quill stream does not persist a text-object → shape link
  (chapter 13 §13.16); when the block and frame counts differ the whole `TEXT`
  array is attached to the first `0x0008` frame.
* The `0x0002`/`0x0003` counted tails are decoded (count pair, element size
  8, box-local vertices) and emitted as custom polygon paths, including for
  image-bearing `0x0002` shapes, whose image fill still binds through the
  owner index.
* The freeform fill/line block (`+0x4E … +0x73` of `0x0003`) is undecoded in
  the specification, so polygons render only when the shared `+0x22`/`+0x23`
  line fields are set.
* Page margins are not part of the 95/97/98 record set, so only the document
  page size is handed to the collector.
* The legacy text pool carries no code page. Pure ASCII pools get an explicit
  `windows-1252` hint; pools with high bytes still go through the collector's
  ICU detector, because forcing `windows-1252` would corrupt the CP1251
  documents (`src/test/data/fdo59355-1.pub` is Russian).


## License

MPL-2.0, unchanged from upstream. The new files carry the same header.
