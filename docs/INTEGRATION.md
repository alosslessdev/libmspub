# Wiring the from-specification model into libmspub

`src/lib/spec/MSPUBSpec.cpp` parses a `.pub` file into a model. This note lists
the exact places where that model should feed libmspub's importer, with the
conditions and offsets from the specification
([alosslessdev/mspub-file-format](https://github.com/alosslessdev/mspub-file-format)).

## 1. Version dispatch — `MSPUBDocument::parse`

Upstream routes on the `Contents[2]` byte:

```
0x22 -> MSPUB_2K  -> Quill present ? MSPUBParser2k : MSPUBParser97
0x2C -> MSPUB_2K2 -> MSPUBParser
```

Two gaps:

* **`0xACE7` magic is rejected.** `getVersion` only accepts `0xACE8`. The loader
  in `MSPUB.EXE` accepts both (spec §02 §2.2, binary VA `0x0045F6B3`).
* **Publisher 98 documents are parsed as 97.** Both write `0x22`; the
  differences are structural, not magic-based (`header_tag` 136 vs 616, page
  slots of type `0x41`, and the record shifts below).

Suggested change: on `MSPUB_2K`, build the model once and use `header_tag`
(`Contents +0x0C`) to pick the layout constants:

| tag | generation | page record | shape record | page slots |
|---|---|---|---|---|
| 125 | 95 | `0x24 + 2n`, descriptor `0x1A` | base | `0x14` |
| 136 | 97 | same as 95 | same as 95 | `0x14` |
| 616 | 98 | `0x28 + 2n`, descriptor `0x1E` | base + 8 (image shapes + 24) | `0x14`, `0x41` |

## 2. Text — `MSPUBCollector::addTextShape` / `addText`

This is the largest gap: upstream requires `Quill/QuillSub/CONTENTS` for text,
which Publisher 95 and 97 do not have.

For those documents the text lives in `Contents`, section
`[u32@0x12, u32@0x16)` (spec §13 §13.14):

1. `store = u32@0x12`; the dword there is `0x002EF0DE` (magic `0xF0DE`, header
   size `0x2E`).
2. Character array: `[u32@(store+0x0E), u32@(store+0x12))`, CP1252, control
   bytes preserved.
3. Story records: three of the four pointers at `store+0x1E`, each
   `u16 0x001D`, `n = u16@+4` (duplicated at `+6`), element size 2 at `+8`,
   `bounds[n]` at `+0x1A`, `ids[n]` at `+0x1A + 4n`, `size == 0x1A + 6n`.
4. Controls: `0x0C` separates frame blocks, `0x0F` separates table cells,
   `0x0D 0x0A` is a paragraph break, `0x05` is a page-number field, `0x0B` a
   soft line break, `0x09` a tab.

The model exposes exactly this (`Document::text`, `Document::stories`,
`Document::cells`).

## 3. Colour — `MSPUBCollector::addPaletteColor`

`0x0047` is a 192-byte record (spec §09 §9.3): scheme name UTF-16LE at `+0x0E`,
eight colour references at `+0xA0`. Resolve each with the reference decoder
(tag classes in §09 §9.1): palette index, literal RGB (`COLORREF`, R in the low
byte), automatic, and the three derived classes.

## 4. Images — `MSPUBCollector::addImage`

`0x0021` is already length-prefixed in upstream; the model adds nothing here
except the parent link (`0x0002`, or the picture object `0x0027`) and the WMF
payload identity (spec §08).

## 5. Embedded objects — new

Upstream ignores `Objects/Object N`. The model lists the storages and the
`0x0022` records (`"MO"` + ordinal at `+0x0A`, flag at `+0x0F`). Note that
`Object N/Contents` is **not** a Publisher document — it is the server's own
format (1 239 of 1 241 are `"WordArt "`), so it must be passed through as an
opaque embedded object, not reparsed (spec §15 §15.9).

## 6. Directory — `MSPUBParser2k::parseContents`

Upstream reads `u16 count`, skips two bytes, then 10-byte records, which yields
correct ids, parents and offsets. Two refinements from the spec:

* the skipped word is `idHighWaterMark` and equals `max(id)`;
* the fifth field of each record is `flags`, not padding, and the record block is
  followed by ten zero bytes (spec §03).

## Test strategy

`tests/expect/*.json` in the specification repository is the target: for each
sample, the chunk inventory, page children, shape count, image payload sizes and
MD5s, palette contents, and text census. A wiring change is correct when the
collector output accounts for every page, shape and text run those files list.
