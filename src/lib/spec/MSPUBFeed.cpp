/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * Feed the dependency-free Publisher 95 / 97 / 98 document model into
 * libmspub's MSPUBCollector.
 *
 * Chapter references (§) point at the specification in
 * https://github.com/alosslessdev/mspub-file-format
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "MSPUBFeed.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <librevenge/librevenge.h>

#include "../ColorReference.h"
#include "../Fill.h"
#include "../Line.h"
#include "../MSPUBCollector.h"
#include "../MSPUBTypes.h"
#include "../PolygonUtils.h"
#include "../Shadow.h"
#include "../ShapeType.h"
#include "../TableInfo.h"

namespace libmspub
{

namespace
{

/* --------------------------------------------------------------- helpers -- */

bool resolveColor(uint32_t ref, uint32_t &out)
{
  uint8_t r = 0, g = 0, b = 0;
  if (!mspub::spec::decodeColorRef(ref, r, g, b))
    return false;
  /* ColorReference's "real color" encoding: low byte R, then G, then B. */
  out = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
  return true;
}

/* Pen width code at +0x22 (chapter 06 §6.7.1), returned in EMU. */
unsigned lineWidthInEmu(uint8_t code)
{
  if (code == 0 || code == 0xff)
    return 0;
  if (code <= 0x7f)
    return unsigned(code) * 12700; // n points, 12 700 EMU per point
  const unsigned k = unsigned(code) - 0x81;
  return ((k % 3 + 1) * 12700) / 4 + (k / 3) * 12700;
}

/* Marker -> geometric primitive.  The model records the marker, not the
   Publisher 2000+ "shape type"; the mapping below mirrors MSPUBParser2k. */
unsigned shapeTypeForMarker(uint16_t marker)
{
  switch (marker)
  {
  case 0x0004:
    return LINE;
  case 0x0007:
    return ELLIPSE;
  default:
    return RECTANGLE;
  }
}

std::vector<std::string> splitBlocks(const std::string &text, char separator)
{
  std::vector<std::string> blocks;
  size_t start = 0;
  while (true)
  {
    const size_t pos = text.find(separator, start);
    blocks.push_back(text.substr(start, pos == std::string::npos ? std::string::npos
                                                                 : pos - start));
    if (pos == std::string::npos)
      break;
    start = pos + 1;
  }
  return blocks;
}

/* Append a UTF-16LE code unit. */
void appendUtf16(std::vector<unsigned char> &out, uint32_t cp)
{
  if (cp < 0x10000)
  {
    out.push_back(static_cast<unsigned char>(cp & 0xff));
    out.push_back(static_cast<unsigned char>((cp >> 8) & 0xff));
  }
  else
  {
    cp -= 0x10000;
    const uint16_t hi = static_cast<uint16_t>(0xd800 + (cp >> 10));
    const uint16_t lo = static_cast<uint16_t>(0xdc00 + (cp & 0x3ff));
    out.push_back(static_cast<unsigned char>(hi & 0xff));
    out.push_back(static_cast<unsigned char>(hi >> 8));
    out.push_back(static_cast<unsigned char>(lo & 0xff));
    out.push_back(static_cast<unsigned char>(lo >> 8));
  }
}

/* The Quill TEXT chunk is decoded to UTF-8 by the model; the collector's
   default text encoding is UTF-16LE, so convert before building spans. */
std::vector<unsigned char> utf8ToUtf16Le(const std::string &in)
{
  std::vector<unsigned char> out;
  out.reserve(in.size() * 2);
  size_t i = 0;
  while (i < in.size())
  {
    const unsigned char c = static_cast<unsigned char>(in[i++]);
    uint32_t cp = 0xfffd;
    if (c < 0x80)
      cp = c;
    else if ((c & 0xe0) == 0xc0 && i < in.size())
      cp = (uint32_t(c & 0x1f) << 6) | (uint32_t(in[i++] & 0x3f));
    else if ((c & 0xf0) == 0xe0 && i + 1 < in.size())
    {
      cp = (uint32_t(c & 0x0f) << 12) | (uint32_t(in[i] & 0x3f) << 6) |
           uint32_t(in[i + 1] & 0x3f);
      i += 2;
    }
    else if ((c & 0xf8) == 0xf0 && i + 2 < in.size())
    {
      cp = (uint32_t(c & 0x07) << 18) | (uint32_t(in[i] & 0x3f) << 12) |
           (uint32_t(in[i + 1] & 0x3f) << 6) | uint32_t(in[i + 2] & 0x3f);
      i += 3;
    }
    appendUtf16(out, cp);
  }
  return out;
}

/* Publisher 95/97 terminate every table cell with CR LF and then write the
   0x0F cell separator (chapter 10 §10.3).  The CR LF already ends the cell's
   paragraph, so leaving it in front of the separator would make 0x0F flush a
   second, empty paragraph.  Strip that terminator, and the one after the
   final separator, before splitting. */
std::string normalizePoolBlock(const std::string &block)
{
  std::string out;
  out.reserve(block.size());
  size_t i = 0;
  while (i < block.size())
  {
    if (block[i] == '\r' && i + 2 < block.size() &&
        block[i + 1] == '\n' && block[i + 2] == '\x0f')
    {
      i += 2;
      continue;
    }
    out.push_back(block[i++]);
  }
  if (out.size() >= 3 && out[out.size() - 3] == '\x0f' &&
      out[out.size() - 2] == '\r' && out[out.size() - 1] == '\n')
    out.resize(out.size() - 2);
  return out;
}

/* Build paragraphs from one frame block.  0x0F separates table cells,
   CR/LF separates paragraphs, 0x0B is a soft line break and 0x05 is a
   page-number field (chapter 13 §13.14.1).  `convert` receives the raw
   paragraph bytes and returns the bytes stored in the span.  When
   `cellEnds` is given, it receives one cumulative character offset per cell
   boundary in the form MSPUBCollector::setTableCellTextEnds() expects (the
   collector starts counting at 1). */
template <typename Converter>
std::vector<TextParagraph> makeParagraphs(const std::string &block, Converter convert,
                                          std::vector<unsigned> *cellEnds = nullptr)
{
  std::vector<TextParagraph> paragraphs;
  std::vector<unsigned char> current;
  unsigned offset = 1;
  auto flush = [&]()
  {
    std::vector<TextSpan> spans;
    spans.push_back(TextSpan(convert(current), CharacterStyle()));
    paragraphs.push_back(TextParagraph(spans, ParagraphStyle()));
    current.clear();
  };
  for (size_t i = 0; i < block.size(); ++i)
  {
    const unsigned char ch = static_cast<unsigned char>(block[i]);
    if (ch == '\r')
    {
      if (i + 1 < block.size() && block[i + 1] == '\n')
        ++i;
      flush();
    }
    else if (ch == '\n')
      flush();
    else if (ch == 0x0f) // table-cell separator
    {
      flush();
      if (cellEnds)
        cellEnds->push_back(offset);
    }
    else if (ch == 0x0b) // soft line break
    {
      current.push_back('\n');
      ++offset;
    }
    else if (ch == 0x05) // page-number field
      continue;
    else
    {
      current.push_back(ch);
      ++offset;
    }
  }
  /* The final field after a trailing separator is not a cell/paragraph. */
  if (!current.empty() || paragraphs.empty())
    flush();
  return paragraphs;
}

std::vector<unsigned char> asBytes(const std::vector<unsigned char> &in)
{
  return in;
}

std::string toString(const std::vector<unsigned char> &in)
{
  return std::string(in.begin(), in.end());
}

/* A 0x0001 frame is a table when the record carries the A/B counts and the
   full 14-byte record arrays (chapter 06 §6.4.1). */
bool isTableShape(const mspub::spec::Shape &s)
{
  return s.type == 0x0001 && s.table.columns > 0 && s.table.rows > 0 &&
         s.table.columnWidthsEmu.size() == s.table.columns &&
         s.table.rowHeightsEmu.size() == s.table.rows;
}

unsigned cellCount(const std::string &block)
{
  return static_cast<unsigned>(std::count(block.begin(), block.end(), '\x0f'));
}

/* A table has no record type of its own: the grid is the A/B counts of the
   owning 0x0001 frame and the cells are the 0x0F-separated runs of its text
   block (chapter 10 §10.2/§10.3).  Cells are stored row-major and the format
   has no merged cells (chapter 10 §10.4), so every cell is 1x1. */
TableInfo makeTableInfo(const mspub::spec::Shape &s)
{
  TableInfo ti(s.table.rows, s.table.columns);
  ti.m_columnWidthsInEmu.assign(s.table.columnWidthsEmu.begin(),
                                s.table.columnWidthsEmu.end());
  ti.m_rowHeightsInEmu.assign(s.table.rowHeightsEmu.begin(),
                              s.table.rowHeightsEmu.end());
  for (unsigned k = 0; k < unsigned(s.table.rows) * unsigned(s.table.columns); ++k)
  {
    CellInfo cell;
    cell.m_startRow = cell.m_endRow = k / s.table.columns;
    cell.m_startColumn = cell.m_endColumn = k % s.table.columns;
    ti.m_cells.push_back(cell);
  }
  return ti;
}

/* ---------------------------------------------------------------- feed ---- */

class Feeder
{
public:
  Feeder(const mspub::spec::Document &doc, MSPUBCollector &collector)
    : m_doc(doc), m_collector(collector), m_shapesById(), m_groupsById(),
      m_pageIds(), m_emitted(), m_imageIndexByOwner(), m_textIdByShape()
  {
    for (const mspub::spec::Shape &s : m_doc.shapes)
      m_shapesById[s.id] = &s;
    for (const mspub::spec::Group &g : m_doc.groups)
      m_groupsById[g.id] = &g;
    for (const mspub::spec::Page &p : m_doc.pages)
      m_pageIds.insert(p.id);
  }

  bool feed()
  {
    if (m_doc.widthEmu == 0 || m_doc.heightEmu == 0)
      return false;

    m_collector.setWidthInEmu(m_doc.widthEmu);
    m_collector.setHeightInEmu(m_doc.heightEmu);

    feedPalette();
    feedImages();
    feedText();
    feedPages();

    return true;
  }

private:
  void feedPalette()
  {
    for (const mspub::spec::Palette &pal : m_doc.palettes)
    {
      for (size_t i = 0; i < pal.refs.size(); ++i)
      {
        uint32_t rgb = 0;
        if (resolveColor(pal.refs[i], rgb))
          m_collector.addPaletteColor(Color(uint8_t(rgb & 0xff),
                                            uint8_t((rgb >> 8) & 0xff),
                                            uint8_t((rgb >> 16) & 0xff)));
        else
          m_collector.addPaletteColor(Color());
      }
    }
  }

  void feedImages()
  {
    unsigned index = 0;
    for (const mspub::spec::Image &img : m_doc.images)
    {
      ++index;
      librevenge::RVNGBinaryData data;
      if (!img.data.empty())
        data.append(img.data.data(), static_cast<unsigned long>(img.data.size()));
      m_collector.addImage(index, WMF, data);

      uint16_t owner = img.parent;
      /* 0x0027 is a picture object: its own directory parent displays it. */
      if (const mspub::spec::Chunk *chunk = chunkById(img.parent))
        if (chunk->type == 0x0027)
          owner = chunk->parent;
      m_imageIndexByOwner[owner] = index;
    }
  }

  const mspub::spec::Chunk *chunkById(uint16_t id) const
  {
    for (const mspub::spec::Chunk &c : m_doc.chunks)
      if (c.id == id)
        return &c;
    return nullptr;
  }

  /* ------------------------------------------------ text: Publisher 95/97 */

  void feedText()
  {
    if (m_doc.hasQuill)
      feedQuillText();
    else
      feedLegacyText();
  }

  void feedLegacyText()
  {
    if (m_doc.text.empty())
      return;

    /* The pool carries no code page.  Chapter 13 §13.14.1 measures CP1252
       over the English corpus, but localized builds write the system ANSI
       code page (the bundled fdo59355-1.pub is Russian CP1251), so forcing
       CP1252 would corrupt those documents.  Pure ASCII is identical under
       every candidate encoding: pin it explicitly instead of running the
       ICU detector on a pool that is too short for it to be reliable.
       Anything with high bytes keeps going through the detector. */
    bool asciiOnly = true;
    for (size_t i = 0; i < m_doc.text.size() && asciiOnly; ++i)
      asciiOnly = (static_cast<unsigned char>(m_doc.text[i]) & 0x80) == 0;
    if (asciiOnly)
      m_collector.setTextEncoding("windows-1252");
    else
      m_collector.useEncodingHeuristic();

    const std::vector<std::string> blocks = splitBlocks(m_doc.text, '\x0c');

    /* The shape side of the link is the u16 text id at shape +0x46: every
       shape with text id i displays block i.  Repeated ids share the block
       (one story flowed across several frames), so group the content shapes
       by text id; the k-th group owns the k-th block.  Shapes with 0xFFFF
       (the master/default frames) and the zero-box gallery defaults own no
       block: the writer numbers only visible text, so leaving them in would
       shift every later shape by one (chapter 10 §10.3.1, chapter 13
       §13.14). */
    std::map<uint16_t, std::vector<unsigned> > groups; // text id -> shape indices
    for (size_t i = 0; i < m_doc.shapes.size(); ++i)
    {
      const mspub::spec::Shape &s = m_doc.shapes[i];
      if ((s.type == 0x0000 || s.type == 0x0001) && s.textId >= 0 &&
          s.right > s.left && s.bottom > s.top)
        groups[uint16_t(s.textId)].push_back(unsigned(i));
    }
    std::vector<uint16_t> order;
    for (std::map<uint16_t, std::vector<unsigned> >::const_iterator it = groups.begin();
         it != groups.end(); ++it)
      order.push_back(it->first);

    for (size_t i = 0; i < blocks.size(); ++i)
    {
      const std::string block = normalizePoolBlock(blocks[i]);
      const mspub::spec::Shape *owner =
        i < order.size() && !groups[order[i]].empty()
        ? &m_doc.shapes[groups[order[i]].front()] : 0;
      /* The table frame is found by its own geometry, not by position:
         the block whose 0x0F cell count equals the frame's A·B is that
         frame's grid (chapter 10 §10.3.1).  Every shape sharing the text
         id gets the same block; only the frame with the matching grid is
         emitted as a table. */
      const mspub::spec::Shape *table = 0;
      if (owner)
      {
        for (size_t k = 0; k < groups[order[i]].size(); ++k)
        {
          const mspub::spec::Shape &cand = m_doc.shapes[groups[order[i]][k]];
          if (isTableShape(cand) &&
              cellCount(block) == unsigned(cand.table.rows) * unsigned(cand.table.columns))
          {
            table = &cand;
            break;
          }
        }
      }
      if (table)
      {
        std::vector<unsigned> cellEnds;
        m_collector.addTextString(makeParagraphs(block, asBytes, &cellEnds),
                                  static_cast<unsigned>(i));
        if (cellEnds.size() == size_t(table->table.rows) * table->table.columns)
        {
          m_collector.setTableCellTextEnds(static_cast<unsigned>(i), cellEnds);
          m_collector.setShapeTableInfo(table->id, makeTableInfo(*table));
        }
      }
      else
        m_collector.addTextString(makeParagraphs(block, asBytes),
                                  static_cast<unsigned>(i));
      if (owner)
      {
        for (size_t k = 0; k < groups[order[i]].size(); ++k)
          m_textIdByShape[m_doc.shapes[groups[order[i]][k]].id] = static_cast<unsigned>(i);
      }
    }
  }

  /* ------------------------------------------------- text: Publisher 98 */

  void feedQuillText()
  {
    if (m_doc.quillText.empty())
      return;

    const std::vector<std::string> blocks = splitBlocks(m_doc.quillText, '\x0c');

    /* Text-frame markers are 0x0008 in Publisher 98 (chapter 13 §13.13). */
    std::vector<uint16_t> frames;
    for (const mspub::spec::Shape &s : m_doc.shapes)
      if (s.type == 0x0008)
        frames.push_back(s.id);
    if (frames.empty() && !m_doc.shapes.empty())
      frames.push_back(m_doc.shapes.front().id);
    if (frames.empty())
      return;

    auto toUtf16 = [](const std::vector<unsigned char> &in)
    {
      return utf8ToUtf16Le(toString(in));
    };

    if (blocks.size() == frames.size())
    {
      for (size_t i = 0; i < blocks.size(); ++i)
      {
        m_collector.addTextString(makeParagraphs(blocks[i], toUtf16),
                                  static_cast<unsigned>(i));
        m_textIdByShape[frames[i]] = static_cast<unsigned>(i);
      }
    }
    else
    {
      /* The Quill stream does not persist a text-object -> shape link
         (chapter 13 §13.16); keep every character by attaching the whole
         array to the first frame. */
      std::string combined;
      for (size_t i = 0; i < blocks.size(); ++i)
      {
        if (i)
          combined += '\r';
        combined += blocks[i];
      }
      m_collector.addTextString(makeParagraphs(combined, toUtf16), 0);
      m_textIdByShape[frames.front()] = 0;
    }
  }

  /* ---------------------------------------------------------- structure -- */

  void feedPages()
  {
    for (const mspub::spec::Page &page : m_doc.pages)
    {
      if (!m_collector.addPage(page.id))
        continue;
      /* A page in the model exists even when it has no shapes: say so, or
         the collector drops it from the drawing. */
      m_collector.setPageHasContent(page.id);
      m_collector.setNextPage(page.id);
      for (size_t i = 0; i < page.children.size(); ++i)
        emitChild(page.children[i], page.id);
    }

    /* Shapes whose directory parent is a page but which the page's z-order
       list omits (settings chunks are the common case) still get emitted, at
       the end of that page. */
    for (const mspub::spec::Shape &s : m_doc.shapes)
    {
      if (m_emitted.find(s.id) != m_emitted.end())
        continue;
      if (m_pageIds.find(s.parent) != m_pageIds.end())
        emitShape(s, s.parent);
    }
    for (const mspub::spec::Group &g : m_doc.groups)
    {
      if (m_emitted.find(g.id) != m_emitted.end())
        continue;
      if (m_pageIds.find(g.parent) != m_pageIds.end())
        emitGroup(g, g.parent);
    }
  }

  void emitChild(uint16_t id, uint16_t pageId)
  {
    if (m_emitted.find(id) != m_emitted.end())
      return;
    std::map<uint16_t, const mspub::spec::Group *>::const_iterator git =
      m_groupsById.find(id);
    if (git != m_groupsById.end())
    {
      emitGroup(*git->second, pageId);
      return;
    }
    std::map<uint16_t, const mspub::spec::Shape *>::const_iterator sit =
      m_shapesById.find(id);
    if (sit != m_shapesById.end())
      emitShape(*sit->second, pageId);
    /* Anything else (a 0x2C page-settings chunk, a picture object, ...) has
       no drawable geometry of its own. */
  }

  void emitGroup(const mspub::spec::Group &g, uint16_t pageId)
  {
    m_emitted.insert(g.id);
    m_collector.setShapePage(g.id, pageId);
    /* Group transforms are not compounded with child transforms in this
       format (chapter 07 §7.4.1), so the group carries no rotation here. */
    m_collector.beginGroup();
    m_collector.setCurrentGroupSeqNum(g.id);
    for (size_t i = 0; i < g.children.size(); ++i)
      emitChild(g.children[i], pageId);
    m_collector.endGroup();
  }

  void emitShape(const mspub::spec::Shape &s, uint16_t pageId)
  {
    m_emitted.insert(s.id);
    m_collector.setShapePage(s.id, pageId);

    /* 0x0002/0x0003 carry a freeform outline: box-local EMU vertices counted
       at +0x74 (chapter 06 §6.4).  Emit it as a custom path so the collector
       draws the polygon instead of its bounding rectangle. */
    const bool polygon = (s.type == 0x0002 || s.type == 0x0003) &&
                         s.vertices.size() >= 3 &&
                         s.right > s.left && s.bottom > s.top;
    m_collector.setShapeType(s.id, polygon ? NOT_PRIMITIVE : shapeTypeForMarker(s.type));
    m_collector.setShapeCoordinatesInEmu(s.id, s.left, s.top, s.right, s.bottom);

    if (polygon)
    {
      DynamicCustomShape shape(unsigned(s.right - s.left), unsigned(s.bottom - s.top));
      shape.m_vertices.reserve(s.vertices.size());
      for (const mspub::spec::Vertex &v : s.vertices)
      {
        Vertex out;
        out.m_x = v.x;
        out.m_y = v.y;
        shape.m_vertices.push_back(out);
      }
      m_collector.setShapeCustomPath(s.id, shape);
    }

    /* The model stores tenths of a degree, counter-clockwise (§6.2).  The
       collector's transform treats a positive angle as clockwise on the
       y-down page, so the complement is the value to hand over; lines have
       redundant rotations and are left at zero (MSPUBParser2k does the
       same). */
    if (s.rotationDeg != 0.0 && s.type != 0x0004)
      m_collector.setShapeRotation(s.id, 360.0 - s.rotationDeg);

    /* Flag word bit 0 is the shadow (§6.7.5): a fixed 1/16-inch down-right
       offset tinted 50 % toward white from the pen colour.  No other flag
       bit flips, frames or mirrors anything (§6.7.6), so only bit 0 is
       emitted. */
    if (s.flags & 0x0001)
    {
      uint32_t shadowRgb = 0;
      if (resolveColor(s.lineColorRef, shadowRgb))
      {
        const int sr = int(shadowRgb & 0xff);
        const int sg = int((shadowRgb >> 8) & 0xff);
        const int sb = int((shadowRgb >> 16) & 0xff);
        const uint32_t tinted =
          uint32_t(uint8_t((12800 + sr * 50) / 100)) |
          (uint32_t(uint8_t((12800 + sg * 50) / 100)) << 8) |
          (uint32_t(uint8_t((12800 + sb * 50) / 100)) << 16);
        m_collector.setShapeShadow(
          s.id, Shadow(OFFSET, 57150, 57150, 0, 0, 0, 0, 1,
                       ColorReference(tinted), ColorReference(tinted)));
      }
    }

    const unsigned width = lineWidthInEmu(s.lineWidthCode);
    uint32_t rgb = 0;
    if (width != 0 && resolveColor(s.lineColorRef, rgb))
      m_collector.addShapeLine(s.id, Line(ColorReference(rgb), width, true));

    if (s.type == 0x0005 && s.fillColorRef != 0 && resolveColor(s.fillColorRef, rgb))
      m_collector.setShapeFill(
        s.id, std::shared_ptr<Fill>(new SolidFill(ColorReference(rgb), 1, &m_collector)),
        false);

    std::map<uint16_t, unsigned>::const_iterator img =
      m_imageIndexByOwner.find(s.id);
    if (img != m_imageIndexByOwner.end())
      m_collector.setShapeImgIndex(s.id, img->second);

    std::map<uint16_t, unsigned>::const_iterator text =
      m_textIdByShape.find(s.id);
    if (text != m_textIdByShape.end())
      m_collector.addTextShape(text->second, s.id);

    m_collector.setShapeOrder(s.id);
  }

  const mspub::spec::Document &m_doc;
  MSPUBCollector &m_collector;
  std::map<uint16_t, const mspub::spec::Shape *> m_shapesById;
  std::map<uint16_t, const mspub::spec::Group *> m_groupsById;
  std::set<uint16_t> m_pageIds;
  std::set<uint16_t> m_emitted;
  std::map<uint16_t, unsigned> m_imageIndexByOwner;
  std::map<uint16_t, unsigned> m_textIdByShape;
};

} // anonymous namespace

bool feedDocument(const mspub::spec::Document &doc, MSPUBCollector &collector)
{
  if (!doc.ok)
    return false;
  Feeder feeder(doc, collector);
  return feeder.feed();
}

} // namespace libmspub

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
