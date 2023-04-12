/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkRefCnt.h"
#include "include/encode/SkWebpEncoder.h"
#include "include/private/base/SkAssert.h"

class GrDirectContext;
class SkData;
class SkImage;
class SkPixmap;
class SkWStream;

namespace SkWebpEncoder {

bool Encode(SkWStream*, const SkPixmap&, const Options&) {
    SkDEBUGFAIL("Using encoder stub");
    return false;
}

sk_sp<SkData> Encode(GrDirectContext*, const SkImage*, const Options&) {
    SkDEBUGFAIL("Using encoder stub");
    return nullptr;
}

}  // namespace SkWebpEncoder
