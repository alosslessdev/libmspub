/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libmspub project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <libmspub/libmspub.h>

#include <memory>
#include <vector>

#include "MSPUBCollector.h"
#include "MSPUBParser.h"
#include "MSPUBParser2k.h"
#include "MSPUBParser97.h"
#include "libmspub_utils.h"
#include "spec/MSPUBFeed.h"
#include "spec/MSPUBSpec.h"

namespace libmspub
{

namespace
{

enum MSPUBVersion
{
  MSPUB_UNKNOWN_VERSION = 0,
  MSPUB_2K,
  MSPUB_2K2
};

MSPUBVersion getVersion(librevenge::RVNGInputStream *input)
{
  try
  {
    if (!input->isStructured())
      return MSPUB_UNKNOWN_VERSION;

    std::unique_ptr<librevenge::RVNGInputStream> contentsStream(input->getSubStreamByName("Contents"));
    if (!contentsStream)
      return MSPUB_UNKNOWN_VERSION;

    const unsigned char magicLow = readU8(contentsStream.get());
    const unsigned char magicHigh = readU8(contentsStream.get());
    /* 0xACE8 is the modern magic; 0xACE7 is the legacy one.  Both are
       accepted by the Publisher 95/97 loader (spec §02 §2.2). */
    if ((0xe8 != magicLow && 0xe7 != magicLow) || 0xac != magicHigh)
      return MSPUB_UNKNOWN_VERSION;

    unsigned char magicVersionByte = readU8(contentsStream.get());

    if (0x00 != readU8(contentsStream.get()))
      return MSPUB_UNKNOWN_VERSION;

    MSPUBVersion version = MSPUB_UNKNOWN_VERSION;
    switch (magicVersionByte)
    {
    case 0x2C:
      version = MSPUB_2K2;
      break;
    case 0x22:
      version =  MSPUB_2K;
      break;
    default:
      break;
    }
    return version;
  }
  catch (...)
  {
    return MSPUB_UNKNOWN_VERSION;
  }

}

bool readAll(librevenge::RVNGInputStream *input, std::vector<unsigned char> &out)
{
  if (!input)
    return false;
  out.clear();
  input->seek(0, librevenge::RVNG_SEEK_SET);
  while (!input->isEnd())
  {
    unsigned long count = 0;
    const unsigned char *block = input->read(16384, count);
    if (!block || count == 0)
      break;
    out.insert(out.end(), block, block + count);
  }
  return !out.empty();
}

} // anonymous namespace



/**
Analyzes the content of an input stream to see if it can be parsed
\param input The input stream
\return A value that indicates whether the content from the input
stream is a Microsoft Publisher Document that libmspub is able to parse
*/
PUBAPI bool MSPUBDocument::isSupported(librevenge::RVNGInputStream *input)
{
  if (!input)
    return false;

  try
  {
    MSPUBVersion version = getVersion(input);
    if (version == MSPUB_UNKNOWN_VERSION)
      return false;

    if (version == MSPUB_2K2)
    {
      std::unique_ptr<librevenge::RVNGInputStream> escherStream(input->getSubStreamByName("Escher/EscherStm"));
      if (!escherStream)
        return false;
      std::unique_ptr<librevenge::RVNGInputStream> quillStream(input->getSubStreamByName("Quill/QuillSub/CONTENTS"));
      if (!quillStream)
        return false;
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
}

/**
Parses the input stream content. It will make callbacks to the functions provided by a
RVNGDrawingInterface class implementation when needed. This is often commonly called the
'main parsing routine'.
\param input The input stream
\param painter A MSPUBPainterInterface implementation
\return A value that indicates whether the parsing was successful
*/
PUBAPI bool MSPUBDocument::parse(librevenge::RVNGInputStream *input, librevenge::RVNGDrawingInterface *painter)
{
  if (!input || !painter)
    return false;

  try
  {
    MSPUBCollector collector(painter);
    input->seek(0, librevenge::RVNG_SEEK_SET);
    std::unique_ptr<MSPUBParser> parser;
    switch (getVersion(input))
    {
    case MSPUB_2K:
    {
      /* Publisher 95/97 keep their text inside Contents and Publisher 98
         uses the shifted records; the legacy parsers lose the former and
         mis-read the latter.  Feed the from-specification model for those
         documents and keep the 2000+ Quill path untouched.
         The model needs the whole OLE2 file: it reads Contents, the Quill
         stream and the object storages itself. */
      std::vector<unsigned char> fileBytes;
      if (readAll(input, fileBytes))
      {
        mspub::spec::Document doc = mspub::spec::parse(fileBytes);
        if (doc.ok && (!doc.hasQuill || doc.headerTag == 616) &&
            feedDocument(doc, collector))
        {
          return collector.go();
        }
      }

      std::unique_ptr<librevenge::RVNGInputStream> quillStream(input->getSubStreamByName("Quill/QuillSub/CONTENTS"));
      if (!quillStream)
        parser.reset(new MSPUBParser97(input, &collector));
      else
        parser.reset(new MSPUBParser2k(input, &collector));
      break;
    }
    case MSPUB_2K2:
    {
      parser.reset(new MSPUBParser(input, &collector));
      break;
    }
    default:
      return false;
    }
    if (parser)
    {
      return parser->parse();
    }
    return false;
  }
  catch (...)
  {
    return false;
  }
}

}

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
