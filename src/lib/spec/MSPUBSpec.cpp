/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * Dependency-free reference implementation of the Microsoft Publisher
 * 95 / 97 / 98 document format.
 *
 * Chapter references (§) point at the specification in
 * https://github.com/alosslessdev/mspub-file-format
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "MSPUBSpec.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace mspub
{
namespace spec
{

namespace
{

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t *p)
{
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
int32_t s32(const uint8_t *p) { return int32_t(u32(p)); }

/* ---------------------------------------------------------------- CFB ---- */

/* Minimal CFB v3/v4 reader (§1). Publisher writes plain MS-CFB: 512-byte
   sectors in every shipped document, 64-byte mini sectors, cutoff 4096. */
class Cfb
{
public:
  bool open(const std::vector<uint8_t> &bytes)
  {
    if (bytes.size() < 512)
      return false;
    static const uint8_t sig[8] = {0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};
    if (std::memcmp(bytes.data(), sig, 8) != 0)
      return false;
    m_bytes = bytes;
    m_sectorSize = uint32_t(1) << u16(&bytes[0x1e]);
    m_miniSectorSize = uint32_t(1) << u16(&bytes[0x20]);
    m_miniCutoff = u32(&bytes[0x38]);
    m_fatSectorCount = u32(&bytes[0x2c]);
    m_firstDirSector = u32(&bytes[0x30]);
    m_firstMiniFat = u32(&bytes[0x3c]);
    m_miniFatCount = u32(&bytes[0x40]);
    m_firstDifat = u32(&bytes[0x44]);
    m_difatCount = u32(&bytes[0x48]);
    if (m_sectorSize < 128 || m_sectorSize > 65536)
      return false;
    m_sectorCount = uint32_t((bytes.size() - 1) / m_sectorSize);
    if (!readFat())
      return false;
    return readDirectory();
  }

  bool readStream(const std::string &name, std::vector<uint8_t> &out) const
  {
    auto it = m_entries.find(name);
    if (it == m_entries.end() || !it->second.isStream)
      return false;
    return readChain(it->second.start, it->second.size, out, it->second.start != 0xFFFFFFFE &&
                     it->second.size < m_miniCutoff && !it->second.isRoot);
  }

  std::vector<std::string> streamNames() const
  {
    std::vector<std::string> names;
    for (const auto &e : m_entries)
      if (e.second.isStream)
        names.push_back(e.first);
    std::sort(names.begin(), names.end());
    return names;
  }

  std::vector<std::string> storagesNamed(const std::string &prefix) const
  {
    std::vector<std::string> out;
    for (const auto &e : m_entries)
      if (e.second.isStorage && e.first.compare(0, prefix.size(), prefix) == 0)
        out.push_back(e.first);
    std::sort(out.begin(), out.end());
    return out;
  }

  bool hasStream(const std::string &name) const
  {
    auto it = m_entries.find(name);
    return it != m_entries.end() && it->second.isStream;
  }

private:
  struct Entry
  {
    bool isStream = false;
    bool isStorage = false;
    bool isRoot = false;
    uint32_t start = 0xFFFFFFFE;
    uint32_t size = 0;
    uint32_t child = 0xFFFFFFFF;
    uint32_t left = 0xFFFFFFFF;
    uint32_t right = 0xFFFFFFFF;
  };

  const uint8_t *sector(uint32_t n) const
  {
    size_t off = (size_t(n) + 1) * m_sectorSize;
    if (off + m_sectorSize > m_bytes.size())
      return nullptr;
    return m_bytes.data() + off;
  }

  bool readFat()
  {
    std::vector<uint32_t> fatSectors;
    for (uint32_t i = 0; i < 109 && fatSectors.size() < m_fatSectorCount; ++i)
    {
      uint32_t s = u32(&m_bytes[0x4c + i * 4]);
      if (s < 0xFFFFFFFA)
        fatSectors.push_back(s);
    }
    uint32_t difat = m_firstDifat;
    uint32_t guard = 0;
    while (difat < 0xFFFFFFFA && m_difatCount && guard++ < m_difatCount + 1)
    {
      const uint8_t *p = sector(difat);
      if (!p)
        break;
      uint32_t per = m_sectorSize / 4 - 1;
      for (uint32_t i = 0; i < per && fatSectors.size() < m_fatSectorCount; ++i)
      {
        uint32_t s = u32(p + i * 4);
        if (s < 0xFFFFFFFA)
          fatSectors.push_back(s);
      }
      difat = u32(p + per * 4);
    }
    m_fat.clear();
    for (uint32_t s : fatSectors)
    {
      const uint8_t *p = sector(s);
      if (!p)
        return false;
      for (uint32_t i = 0; i < m_sectorSize / 4; ++i)
        m_fat.push_back(u32(p + i * 4));
    }
    return !m_fat.empty();
  }

  bool readDirectory()
  {
    std::vector<uint8_t> dir;
    if (!readChain(m_firstDirSector, 0xFFFFFFFE, dir, false))
      return false;
    const size_t entrySize = 128;
    std::vector<Entry> entries;
    std::vector<std::string> names;
    for (size_t off = 0; off + entrySize <= dir.size(); off += entrySize)
    {
      const uint8_t *p = &dir[off];
      Entry e;
      uint16_t nameLen = u16(p + 0x40);
      uint8_t type = p[0x42];
      e.isStream = type == 2;
      e.isStorage = type == 1;
      e.isRoot = type == 5;
      e.left = u32(p + 0x44);
      e.right = u32(p + 0x48);
      e.child = u32(p + 0x4c);
      e.start = u32(p + 0x74);
      e.size = u32(p + 0x78);
      std::string name;
      if (nameLen >= 2 && nameLen <= 64)
      {
        for (uint16_t i = 0; i + 1 < nameLen; i += 2)
        {
          uint16_t c = u16(p + i);
          if (!c)
            break;
          name.push_back(c < 0x80 ? char(c) : '?');
        }
      }
      entries.push_back(e);
      names.push_back(name);
    }
    if (entries.empty())
      return false;
    /* Walk the red-black tree from the root's child pointer. */
    std::vector<std::pair<uint32_t, std::string> > stack;
    std::vector<bool> seen(entries.size(), false);
    uint32_t root = 0;
    for (uint32_t i = 0; i < entries.size(); ++i)
      if (entries[i].isRoot)
        root = i;
    m_entries.clear();
    m_entries[""].isRoot = true;
    /* The mini stream lives in the root entry; capture it before walking,
       because the root itself is not visited by the traversal below. */
    m_miniStreamStart = entries[root].start;
    m_miniStreamSize = entries[root].size;
    stack.push_back(std::make_pair(entries[root].child, std::string()));
    while (!stack.empty())
    {
      uint32_t idx = stack.back().first;
      std::string prefix = stack.back().second;
      stack.pop_back();
      if (idx >= entries.size() || seen[idx])
        continue;
      seen[idx] = true;
      const Entry &e = entries[idx];
      std::string path = prefix.empty() ? names[idx] : prefix + "/" + names[idx];
      Entry copy = e;
      m_entries[path] = copy;
      if (e.right < entries.size())
        stack.push_back(std::make_pair(e.right, prefix));
      if (e.left < entries.size())
        stack.push_back(std::make_pair(e.left, prefix));
      if ((e.isStorage || e.isRoot) && e.child < entries.size())
        stack.push_back(std::make_pair(e.child, path));
    }
    /* Mini FAT */
    m_miniFat.clear();
    if (m_firstMiniFat < 0xFFFFFFFA && m_miniFatCount)
    {
      std::vector<uint8_t> mf;
      if (readChain(m_firstMiniFat, 0xFFFFFFFE, mf, false))
        for (size_t i = 0; i + 4 <= mf.size(); i += 4)
          m_miniFat.push_back(u32(&mf[i]));
    }
    return true;
  }

  bool readChain(uint32_t start, uint32_t size, std::vector<uint8_t> &out, bool mini) const
  {
    out.clear();
    if (mini)
    {
      std::vector<uint8_t> miniStream;
      if (!readMiniStream(miniStream))
        return false;
      uint32_t sec = start;
      uint32_t guard = 0;
      while (sec < 0xFFFFFFFA && guard++ < m_miniFat.size() + 1)
      {
        size_t off = size_t(sec) * m_miniSectorSize;
        if (off + m_miniSectorSize > miniStream.size())
          break;
        out.insert(out.end(), miniStream.begin() + off, miniStream.begin() + off + m_miniSectorSize);
        if (sec >= m_miniFat.size())
          break;
        sec = m_miniFat[sec];
      }
    }
    else
    {
      uint32_t sec = start;
      uint32_t guard = 0;
      while (sec < 0xFFFFFFFA && guard++ < m_fat.size() + 1)
      {
        const uint8_t *p = sector(sec);
        if (!p)
          break;
        out.insert(out.end(), p, p + m_sectorSize);
        if (sec >= m_fat.size())
          break;
        sec = m_fat[sec];
      }
    }
    if (size != 0xFFFFFFFE && out.size() > size)
      out.resize(size);
    return true;
  }

  bool readMiniStream(std::vector<uint8_t> &out) const
  {
    if (!m_miniStreamCache.empty() || m_miniStreamSize == 0)
    {
      out = m_miniStreamCache;
      return true;
    }
    std::vector<uint8_t> tmp;
    if (!readChain(m_miniStreamStart, m_miniStreamSize, tmp, false))
      return false;
    m_miniStreamCache = tmp;
    out = m_miniStreamCache;
    return true;
  }

  std::vector<uint8_t> m_bytes;
  uint32_t m_sectorSize = 512;
  uint32_t m_miniSectorSize = 64;
  uint32_t m_miniCutoff = 4096;
  uint32_t m_sectorCount = 0;
  uint32_t m_fatSectorCount = 0;
  uint32_t m_firstDirSector = 0;
  uint32_t m_firstMiniFat = 0xFFFFFFFE;
  uint32_t m_miniFatCount = 0;
  uint32_t m_firstDifat = 0xFFFFFFFE;
  uint32_t m_difatCount = 0;
  uint32_t m_miniStreamStart = 0xFFFFFFFE;
  uint32_t m_miniStreamSize = 0;
  std::vector<uint32_t> m_fat;
  std::vector<uint32_t> m_miniFat;
  mutable std::vector<uint8_t> m_miniStreamCache;
  std::map<std::string, Entry> m_entries;
};

/* ------------------------------------------------------------- helpers ---- */

std::string utf16leToUtf8(const uint8_t *p, size_t bytes)
{
  std::string out;
  for (size_t i = 0; i + 1 < bytes; i += 2)
  {
    uint32_t c = u16(p + i);
    if (!c)
      break;
    if (c < 0x80)
      out.push_back(char(c));
    else if (c < 0x800)
    {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
    else
    {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

std::string jsonEscape(const std::string &s)
{
  std::string out;
  for (unsigned char c : s)
  {
    switch (c)
    {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20)
      {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      }
      else
        out.push_back(char(c));
    }
  }
  return out;
}

/* Markers 0x0000…0x000B are shape records; 0x000C…0x000E and 0x0010+ are
   not (0x000F is the group record and 0x0016 a document-level record). */
bool isShapeMarker(uint16_t t) { return t <= 0x000b; }

} // namespace

/* ------------------------------------------------------------ document ---- */

std::vector<Chunk> Document::chunksOfType(uint16_t type) const
{
  std::vector<Chunk> out;
  for (const Chunk &c : chunks)
    if (c.type == type)
      out.push_back(c);
  return out;
}

Document parse(const std::vector<uint8_t> &file)
{
  Document doc;
  Cfb cfb;
  if (!cfb.open(file))
  {
    doc.error = "not a CFB container";
    return doc;
  }
  std::vector<uint8_t> contents;
  if (!cfb.readStream("Contents", contents) || contents.size() < 0x22)
  {
    doc.error = "no Contents stream";
    return doc;
  }

  doc.magic = u16(&contents[0]);
  doc.versionByte = contents[2];
  doc.formatVersion = u16(&contents[4]);
  doc.contentsLength = uint32_t(contents.size());
  doc.headerTag = u16(&contents[0x0c]);
  doc.textSectionOffset = u32(&contents[0x12]);
  doc.textSectionEnd = u32(&contents[0x16]);

  if (doc.magic != 0xace8 && doc.magic != 0xace7)
  {
    doc.error = "bad Contents magic";
    return doc;
  }

  /* Directory (§3): u16 count, u16 idHighWaterMark, records, 10 zero bytes. */
  const uint32_t dirOff = doc.textSectionEnd;
  if (dirOff + 4 > contents.size())
  {
    doc.error = "directory offset out of range";
    return doc;
  }
  const uint16_t count = u16(&contents[dirOff]);
  std::vector<Chunk> recs;
  size_t p = dirOff + 4;
  for (uint16_t i = 0; i < count && p + 10 <= contents.size(); ++i, p += 10)
  {
    Chunk c;
    c.id = u16(&contents[p]);
    c.parent = u16(&contents[p + 2]);
    c.offset = u32(&contents[p + 4]);
    c.flags = u16(&contents[p + 8]);
    recs.push_back(c);
  }
  /* Size = next offset - own; chunks are stored in ascending order. */
  std::vector<Chunk> sorted = recs;
  std::sort(sorted.begin(), sorted.end(),
            [](const Chunk &a, const Chunk &b) { return a.offset < b.offset; });
  for (size_t i = 0; i < sorted.size(); ++i)
  {
    uint32_t end = (i + 1 < sorted.size()) ? sorted[i + 1].offset : uint32_t(contents.size());
    for (Chunk &c : recs)
      if (c.offset == sorted[i].offset)
      {
        c.size = end > c.offset ? end - c.offset : 0;
        if (c.offset + 2 <= contents.size())
          c.type = u16(&contents[c.offset]);
        break;
      }
  }
  doc.chunks = recs;

  /* DOCUMENT (§4): exactly one chunk of type 0x0015. */
  for (const Chunk &c : doc.chunks)
    if (c.type == 0x0015)
    {
      doc.documentId = c.id;
      doc.coordinateSystem = c.size >= 0x14 ? u16(&contents[c.offset + 0x12]) : 0;
      doc.widthEmu = c.size >= 0x1c ? u32(&contents[c.offset + 0x14]) : 0;
      doc.heightEmu = c.size >= 0x20 ? u32(&contents[c.offset + 0x18]) : 0;
      break;
    }

  /* Pages (§5): type 0x0014, plus 0x0041 page slots in Publisher 98. */
  for (const Chunk &c : doc.chunks)
  {
    if (c.type != 0x0014 && c.type != 0x0041)
      continue;
    const uint8_t *base = &contents[c.offset];
    if (c.size < 0x24)
      continue;
    const bool p98 = c.size >= 0x24 && base[2] == 0x1e;
    Page page;
    page.id = c.id;
    page.type = c.type;
    const size_t cntOff = p98 ? 0x1e : 0x1a;
    const size_t kindOff = p98 ? 0x1c : 0x18;
    const size_t arrOff = p98 ? 0x28 : 0x24;
    page.count = u16(base + cntOff);
    page.kind = u16(base + kindOff);
    if (arrOff + 2 * size_t(page.count) <= c.size)
      for (uint16_t i = 0; i < page.count; ++i)
        page.children.push_back(u16(base + arrOff + 2 * i));
    doc.pages.push_back(page);
  }

  /* Groups (§7): geometry is the same block as a shape; the child list sits
     at +0x18/+0x22 and moves by +8 for the Publisher 98 layout. */
  for (const Chunk &c : doc.chunks)
  {
    if (c.type != 0x000f || c.size < 0x16)
      continue;
    const uint8_t *base = &contents[c.offset];
    const bool p98 = c.size >= 0x20 && base[2] == 0x20;
    const size_t cntOff = p98 ? 0x20 : 0x18;
    const size_t arrOff = p98 ? 0x2a : 0x22;
    Group g;
    g.id = c.id;
    g.parent = c.parent;
    g.rotationDeg = u16(base + 4) / 10.0;
    g.left = s32(base + 6);
    g.top = s32(base + 0x0a);
    g.right = s32(base + 0x0e);
    g.bottom = s32(base + 0x12);
    const uint16_t n = u16(base + cntOff);
    if (arrOff + 2 * size_t(n) <= c.size)
      for (uint16_t i = 0; i < n; ++i)
        g.children.push_back(u16(base + arrOff + 2 * i));
    doc.groups.push_back(g);
  }

  /* Shapes (§6): geometry at +0x04/+0x06 for every shape marker. */
  for (const Chunk &c : doc.chunks)
  {
    if (!isShapeMarker(c.type) || c.size < 0x16)
      continue;
    const uint8_t *base = &contents[c.offset];
    Shape s;
    s.id = c.id;
    s.parent = c.parent;
    s.type = c.type;
    s.rotationDeg = u16(base + 4) / 10.0;
    s.left = s32(base + 6);
    s.top = s32(base + 0x0a);
    s.right = s32(base + 0x0e);
    s.bottom = s32(base + 0x12);

    /* The draw-time fields are only mapped for the 95/97 record layout; the
       descriptor byte B is the per-marker constant (chapter 06 §6.3).  The
       Publisher 98 records carry B+8 and are deliberately left alone. */
    const uint8_t b = c.size > 3 ? base[3] : 0;
    const bool legacy = b == 0x5c || b == 0x60 || b == 0x74 || b == 0x3b ||
                        b == 0x3e || b == 0x35 || b == 0x29;
    if (legacy)
    {
      if (c.size >= 0x27)
      {
        s.lineWidthCode = base[0x22];
        s.lineColorRef = u32(base + 0x23);
      }
      if (c.type == 0x0005 && c.size >= 0x32)
        s.fillColorRef = u32(base + 0x2e);
      if ((c.type == 0x0000 || c.type == 0x0001) && c.size >= 0x48)
      {
        const uint16_t textId = u16(base + 0x46);
        s.textId = textId == 0xffff ? -1 : int(textId);
      }
    }
    doc.shapes.push_back(s);
  }

  /* Images (§8): 0x0021 is a length-prefixed WMF. */
  for (const Chunk &c : doc.chunks)
  {
    if (c.type != 0x0021 || c.size < 8)
      continue;
    const uint8_t *base = &contents[c.offset];
    uint32_t n = u32(base + 4);
    if (n > c.size - 8)
      n = c.size - 8;
    Image img;
    img.id = c.id;
    img.parent = c.parent;
    img.data.assign(base + 8, base + 8 + n);
    doc.images.push_back(img);
  }

  /* Palette (§9): 192 bytes, UTF-16LE scheme name, eight colour references. */
  for (const Chunk &c : doc.chunks)
  {
    if (c.type != 0x0047 || c.size < 0xc0)
      continue;
    const uint8_t *base = &contents[c.offset];
    Palette pal;
    pal.id = c.id;
    pal.scheme = utf16leToUtf8(base + 0x0e, std::min<size_t>(0x82, c.size - 0x0e));
    for (size_t i = 0; i < 8; ++i)
      pal.refs[i] = u32(base + 0xa0 + 4 * i);
    doc.palettes.push_back(pal);
  }

  /* Embedded-object references (§15.8). */
  for (const Chunk &c : doc.chunks)
  {
    if (c.type != 0x0022 || c.size < 0x12)
      continue;
    const uint8_t *base = &contents[c.offset];
    ObjectRef ref;
    ref.shapeId = c.parent;
    ref.ordinal = u16(base + 0x0a);
    ref.flag = base[0x0f];
    doc.objects.push_back(ref);
  }

  /* Embedded storages (§15). */
  doc.objectStorages = cfb.storagesNamed("Objects/Object ");

  /* Publisher 95/97 text store (§13.14). */
  if (doc.textSectionOffset + 0x2e <= contents.size() &&
      u32(&contents[doc.textSectionOffset]) == 0x002ef0de)
  {
    const uint8_t *store = &contents[doc.textSectionOffset];
    const uint32_t textBase = u32(store + 0x0e);
    const uint32_t textEnd = u32(store + 0x12);
    if (textBase <= textEnd && textEnd <= contents.size())
      doc.text.assign(reinterpret_cast<const char *>(&contents[textBase]),
                      reinterpret_cast<const char *>(&contents[textEnd]));
    for (size_t i = 0; i < 4; ++i)
    {
      const uint32_t ptr = u32(store + 0x1e + 4 * i);
      if (!ptr || ptr + 0x1a > contents.size())
        continue;
      const uint8_t *rec = &contents[ptr];
      if (u16(rec) != 0x001d)
        continue;
      Story st;
      const uint16_t n = u16(rec + 4);
      st.textStart = u32(rec + 0x16);
      if (ptr + 0x1a + 6 * size_t(n) <= contents.size())
      {
        for (uint16_t k = 0; k < n; ++k)
        {
          st.bounds.push_back(u32(rec + 0x1a + 4 * k));
          st.paraIds.push_back(u16(rec + 0x1a + 4 * n + 2 * k));
        }
      }
      doc.stories.push_back(st);
    }
  }

  /* Table cells (§10.3): 0x0C separates blocks, 0x0F separates cells. */
  if (!doc.text.empty())
  {
    size_t block = 0;
    size_t start = 0;
    while (start <= doc.text.size())
    {
      size_t end = doc.text.find('\x0c', start);
      std::string chunk = doc.text.substr(start, end == std::string::npos ? std::string::npos
                                                                         : end - start);
      std::vector<std::string> cells;
      size_t cs = 0;
      while (true)
      {
        size_t ce = chunk.find('\x0f', cs);
        cells.push_back(chunk.substr(cs, ce == std::string::npos ? std::string::npos : ce - cs));
        if (ce == std::string::npos)
          break;
        cs = ce + 1;
      }
      if (cells.size() >= 4)
      {
        CellBlock cb;
        cb.blockIndex = block;
        cb.cells = cells.size();
        cb.text = cells;
        doc.cells.push_back(cb);
      }
      ++block;
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
  }

  /* Publisher 98 Quill text engine (§13). */
  if (cfb.hasStream("Quill/QuillSub/CONTENTS"))
  {
    std::vector<uint8_t> quill;
    if (cfb.readStream("Quill/QuillSub/CONTENTS", quill) && quill.size() >= 0x20)
    {
      doc.hasQuill = true;
      uint32_t off = 0x18;
      size_t guard = 0;
      while (off != 0xFFFFFFFF && off + 8 <= quill.size() && guard++ < 64)
      {
        const uint16_t n = u16(&quill[off + 2]);
        const uint32_t next = u32(&quill[off + 4]);
        size_t rp = off + 8;
        for (uint16_t i = 0; i < n && rp + 24 <= quill.size(); ++i, rp += 24)
        {
          std::string name(reinterpret_cast<const char *>(&quill[rp + 2]), 4);
          while (!name.empty() && name.back() == ' ')
            name.pop_back();
          const uint32_t dataOff = u32(&quill[rp + 0x10]);
          const uint32_t dataLen = u32(&quill[rp + 0x14]);
          doc.quillChunks.push_back(name);
          if (name == "TEXT" && dataOff + dataLen <= quill.size() && doc.quillText.empty())
            doc.quillText = utf16leToUtf8(&quill[dataOff], dataLen);
        }
        off = next;
      }
    }
  }

  doc.ok = true;
  return doc;
}

bool parseFile(const std::string &path, Document &out)
{
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0)
  {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size), 0);
  size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  if (got != bytes.size())
    return false;
  out = parse(bytes);
  return out.ok;
}

bool decodeColorRef(uint32_t ref, uint8_t &r, uint8_t &g, uint8_t &b)
{
  /* Static Office palette at VA 0x00600060 (chapter 09 §9.2); the 56 entries
     0x00…0x37 are plain COLORREFs, so R is the low byte. */
  static const uint32_t table[56] =
  {
    0x00000000, 0x00ffffff, 0x000000ff, 0x0000ff00, 0x00ff0000, 0x0000ffff, 0x00ffff00, 0x00ff00ff,
    0x00808080, 0x00c0c0c0, 0x00000080, 0x00008000, 0x00800000, 0x00008080, 0x00808000, 0x00800080,
    0x003399ff, 0x00330033, 0x00990000, 0x00009900, 0x00009999, 0x000066cc, 0x00000099, 0x00cc99cc,
    0x00ff6666, 0x0066ff66, 0x0066ffff, 0x0099ccff, 0x006666ff, 0x000099ff, 0x00ff6600, 0x0000ccff,
    0x00330099, 0x00003366, 0x00424242, 0x006699ff, 0x00003399, 0x000066ff, 0x00003333, 0x0000cc99,
    0x0099ffff, 0x00003300, 0x00669933, 0x00ccffcc, 0x00663300, 0x00cccc33, 0x00ffffcc, 0x00ff6633,
    0x00ffcc00, 0x00ffcc99, 0x00993333, 0x00996666, 0x00663399, 0x00ff99cc, 0x00333333, 0x00969696
  };
  const uint8_t tag = uint8_t((ref >> 24) & 0xFF);
  const unsigned top = tag >> 5;
  if (top == 0 || top == 6)
  {
    /* Palette index (top 0), or the brightness-modified palette entry
       (top 6, whose low word is also an index).  Resolve the index through
       the static table; brightness modulation is not applied here. */
    const uint32_t index = ref & 0xFFFF;
    if (index >= 56)
      return false;
    const uint32_t entry = table[index];
    r = uint8_t(entry & 0xFF);
    g = uint8_t((entry >> 8) & 0xFF);
    b = uint8_t((entry >> 16) & 0xFF);
    return true;
  }
  if (top == 1)
  {
    r = uint8_t(ref & 0xFF);
    g = uint8_t((ref >> 8) & 0xFF);
    b = uint8_t((ref >> 16) & 0xFF);
    return true;
  }
  if (top == 5 && (ref & 0xFFFF) == 0)
  {
    r = 0xD7;
    g = 0xB7;
    b = 0x73;
    return true;
  }
  /* Derived colour spaces (tags 0b010 / 0b100) need tables the model does
     not carry; report failure rather than inventing a colour. */
  return false;
}

std::string toJson(const Document &doc)
{
  std::ostringstream o;
  o << "{\n";
  o << "  \"ok\": " << (doc.ok ? "true" : "false") << ",\n";
  if (!doc.ok)
    o << "  \"error\": \"" << jsonEscape(doc.error) << "\",\n";
  o << "  \"magic\": " << doc.magic << ",\n";
  o << "  \"versionByte\": " << unsigned(doc.versionByte) << ",\n";
  o << "  \"formatVersion\": " << doc.formatVersion << ",\n";
  o << "  \"headerTag\": " << doc.headerTag << ",\n";
  o << "  \"contentsLength\": " << doc.contentsLength << ",\n";
  o << "  \"textSection\": [" << doc.textSectionOffset << ", " << doc.textSectionEnd << "],\n";
  o << "  \"pageSizeEmu\": [" << doc.widthEmu << ", " << doc.heightEmu << "],\n";
  o << "  \"coordinateSystem\": " << doc.coordinateSystem << ",\n";
  o << "  \"chunks\": " << doc.chunks.size() << ",\n";
  o << "  \"pages\": " << doc.pages.size() << ",\n";
  o << "  \"shapes\": " << doc.shapes.size() << ",\n";
  o << "  \"groups\": " << doc.groups.size() << ",\n";
  o << "  \"images\": " << doc.images.size() << ",\n";
  o << "  \"objects\": " << doc.objects.size() << ",\n";
  o << "  \"objectStorages\": " << doc.objectStorages.size() << ",\n";
  o << "  \"stories\": " << doc.stories.size() << ",\n";
  o << "  \"textBytes\": " << doc.text.size() << ",\n";
  o << "  \"cellBlocks\": " << doc.cells.size() << ",\n";
  o << "  \"quill\": " << (doc.hasQuill ? "true" : "false") << ",\n";
  if (doc.hasQuill)
  {
    o << "  \"quillTextChars\": " << doc.quillText.size() << ",\n";
    o << "  \"quillChunks\": [";
    for (size_t i = 0; i < doc.quillChunks.size(); ++i)
      o << (i ? ", " : "") << "\"" << jsonEscape(doc.quillChunks[i]) << "\"";
    o << "],\n";
  }
  o << "  \"palettes\": [";
  for (size_t i = 0; i < doc.palettes.size(); ++i)
  {
    const Palette &p = doc.palettes[i];
    o << (i ? ", " : "") << "{\"id\": " << p.id << ", \"scheme\": \"" << jsonEscape(p.scheme)
      << "\", \"refs\": [";
    for (size_t k = 0; k < 8; ++k)
      o << (k ? ", " : "") << p.refs[k];
    o << "]}";
  }
  o << "],\n";
  o << "  \"pageChildren\": [";
  for (size_t i = 0; i < doc.pages.size(); ++i)
    o << (i ? ", " : "") << doc.pages[i].children.size();
  o << "]\n}\n";
  return o.str();
}

} // namespace spec
} // namespace mspub

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
