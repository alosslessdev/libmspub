/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * Feed the dependency-free Publisher 95 / 97 / 98 document model into
 * libmspub's MSPUBCollector.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#ifndef INCLUDED_MSPUBFEED_H
#define INCLUDED_MSPUBFEED_H

#include "MSPUBSpec.h"

namespace libmspub
{

class MSPUBCollector;

/** Translate a parsed model into collector calls.
 *
 * The caller owns the collector; the function only feeds it (it does not call
 * MSPUBCollector::go()).  Returns false when the model cannot be used at all.
 */
bool feedDocument(const mspub::spec::Document &doc, MSPUBCollector &collector);

} // namespace libmspub

#endif // INCLUDED_MSPUBFEED_H

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
