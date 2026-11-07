/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * Dependency-free reference implementation of the Microsoft Publisher
 * 95 / 97 / 98 document format, derived from the specification published at
 * https://github.com/alosslessdev/mspub-file-format
 *
 * This code deliberately has no external dependencies: it operates on the raw
 * bytes of a .pub file so that the format logic can be built and tested
 * anywhere, including in CI without librevenge.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#ifndef INCLUDED_MSPUB_SPEC_H
#define INCLUDED_MSPUB_SPEC_H

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mspub
{
namespace spec
{

/** One chunk-directory record (chapter 03). */
struct Chunk
{
  uint16_t id = 0;
  uint16_t parent = 0;
  uint16_t type = 0;
  uint16_t flags = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
};

/** A shape record with the four geometry fields (chapter 06). */
struct Shape
{
  uint16_t id = 0;
  uint16_t parent = 0;
  uint16_t type = 0;
  double rotationDeg = 0.0;   // stored 1/10 degree, counter-clockwise
  int32_t left = 0, top = 0, right = 0, bottom = 0;  // EMU

  // Publisher 95/97 draw-time fields (chapter 06 section 6.7).  These are
  // only meaningful when the record uses the 95/97 descriptor gate; for the
  // Publisher 98 layout the fields move and are left at their defaults.
  int textId = -1;            // u16 at +0x46 (0xFFFF = no text), 0x0000/0x0001
  uint8_t lineWidthCode = 0;  // u8 at +0x22
  uint32_t lineColorRef = 0;  // u32 at +0x23
  uint32_t fillColorRef = 0;  // u32 at +0x2E (0x0005 only)
};

/** A group record (chapter 07): z-ordered child chunk ids. */
struct Group
{
  uint16_t id = 0;
  uint16_t parent = 0;
  double rotationDeg = 0.0;   // stored 1/10 degree, counter-clockwise
  int32_t left = 0, top = 0, right = 0, bottom = 0;  // EMU
  std::vector<uint16_t> children;
};

/** A page slot: type 0x0014, or 0x0041 in Publisher 98 (chapter 05). */
struct Page
{
  uint16_t id = 0;
  uint16_t type = 0x0014;
  uint16_t kind = 0;
  uint16_t count = 0;
  std::vector<uint16_t> children;  // z-order
};

/** A text story record (chapter 13 §13.14.2). */
struct Story
{
  uint16_t id = 0;
  uint32_t textStart = 0;
  std::vector<uint32_t> bounds;
  std::vector<uint16_t> paraIds;
};

/** An embedded image (chapter 08). */
struct Image
{
  uint16_t id = 0;
  uint16_t parent = 0;
  std::vector<uint8_t> data;  // a complete WMF payload
};

/** The document palette (chapter 09). */
struct Palette
{
  uint16_t id = 0;
  std::string scheme;              // UTF-16LE name at +0x0E
  std::array<uint32_t, 8> refs {}; // colour references at +0xA0
};

/** An embedded-object reference (chapter 15 §15.8). */
struct ObjectRef
{
  uint16_t shapeId = 0;
  uint16_t ordinal = 0;
  uint8_t flag = 0;
};

/** A table cell block reconstructed from the text pool (chapter 10). */
struct CellBlock
{
  size_t blockIndex = 0;
  size_t cells = 0;
  std::vector<std::string> text;
};

struct Document
{
  bool ok = false;
  std::string error;

  // container / header (chapters 01, 02)
  uint16_t magic = 0;
  uint8_t versionByte = 0;
  uint16_t formatVersion = 0;
  uint16_t headerTag = 0;
  uint32_t contentsLength = 0;
  uint32_t textSectionOffset = 0;
  uint32_t textSectionEnd = 0;

  // document record (chapter 04)
  uint16_t documentId = 0;
  uint16_t coordinateSystem = 0;
  uint32_t widthEmu = 0;
  uint32_t heightEmu = 0;

  std::vector<Chunk> chunks;
  std::vector<Page> pages;
  std::vector<Shape> shapes;
  std::vector<Group> groups;
  std::vector<Image> images;
  std::vector<Palette> palettes;
  std::vector<ObjectRef> objects;
  std::vector<CellBlock> cells;

  // Publisher 95/97 text store (chapter 13 §13.14)
  std::string text;                 // CP1252, control bytes preserved
  std::vector<Story> stories;

  // Publisher 98 Quill (chapter 13)
  bool hasQuill = false;
  std::string quillText;            // UTF-16LE decoded to UTF-8
  std::vector<std::string> quillChunks;

  // embedded objects (chapter 15)
  std::vector<std::string> objectStorages;

  /** Chunks of one type marker, in directory order. */
  std::vector<Chunk> chunksOfType(uint16_t type) const;
  /** True when the document carries a Quill text engine stream. */
  bool usesQuill() const { return hasQuill; }
};

/** Parse a complete .pub file held in memory. */
Document parse(const std::vector<uint8_t> &file);

/** Convenience: read a file from disk and parse it. Returns false on I/O error. */
bool parseFile(const std::string &path, Document &out);

/** Colour reference -> RGB (chapter 09 §9.1). Returns false when the reference
    needs a table this module does not carry (derived/automatic colours). */
bool decodeColorRef(uint32_t ref, uint8_t &r, uint8_t &g, uint8_t &b);

/** Serialise a document as JSON (used by the explore CLI). */
std::string toJson(const Document &doc);

} // namespace spec
} // namespace mspub

#endif // INCLUDED_MSPUB_SPEC_H

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
