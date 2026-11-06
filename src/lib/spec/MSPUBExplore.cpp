/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * pubexplore — dump a Microsoft Publisher 95/97/98 document as JSON.
 *
 * This is the CLI for the dependency-free implementation of the format
 * specification in https://github.com/alosslessdev/mspub-file-format
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "MSPUBSpec.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void usage()
{
  std::fprintf(stderr,
               "usage: pubexplore [options] <file.pub>...\n"
               "  --json              print a JSON summary (default)\n"
               "  --dump-images DIR   write every embedded image as a .wmf file\n"
               "  --quiet             only report failures\n");
}

int main(int argc, char **argv)
{
  bool json = true;
  bool quiet = false;
  std::string imageDir;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i)
  {
    if (!std::strcmp(argv[i], "--json"))
      json = true;
    else if (!std::strcmp(argv[i], "--quiet"))
      quiet = true;
    else if (!std::strcmp(argv[i], "--dump-images") && i + 1 < argc)
      imageDir = argv[++i];
    else if (!std::strcmp(argv[i], "--help"))
    {
      usage();
      return 0;
    }
    else
      files.push_back(argv[i]);
  }
  if (files.empty())
  {
    usage();
    return 2;
  }

  int failures = 0;
  for (const std::string &path : files)
  {
    mspub::spec::Document doc;
    if (!mspub::spec::parseFile(path, doc))
    {
      std::fprintf(stderr, "%s: %s\n", path.c_str(), doc.error.empty() ? "parse failed" : doc.error.c_str());
      ++failures;
      continue;
    }
    if (!quiet)
      std::fputs(mspub::spec::toJson(doc).c_str(), stdout);

    if (!imageDir.empty())
    {
      for (size_t i = 0; i < doc.images.size(); ++i)
      {
        char name[512];
        std::snprintf(name, sizeof name, "%s/%04zu.wmf", imageDir.c_str(), i);
        std::FILE *f = std::fopen(name, "wb");
        if (!f)
        {
          std::fprintf(stderr, "%s: cannot write\n", name);
          ++failures;
          continue;
        }
        const mspub::spec::Image &img = doc.images[i];
        std::fwrite(img.data.data(), 1, img.data.size(), f);
        std::fclose(f);
      }
    }
  }
  return failures ? 1 : 0;
}

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
