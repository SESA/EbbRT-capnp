// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "serialize.h"
#include "layout.h"
#include <kj/debug.h>
#include <exception>

namespace capnp {

FlatArrayMessageReader::FlatArrayMessageReader(
    kj::ArrayPtr<const word> array, ReaderOptions options)
    : MessageReader(options) {
  if (array.size() < 1) {
    // Assume empty message.
    return;
  }

  const _::WireValue<uint32_t>* table =
      reinterpret_cast<const _::WireValue<uint32_t>*>(array.begin());

  uint segmentCount = table[0].get() + 1;
  size_t offset = segmentCount / 2u + 1u;

  KJ_REQUIRE(array.size() >= offset, "Message ends prematurely in segment table.") {
    return;
  }

  if (segmentCount == 0) {
    return;
  }

  uint segmentSize = table[1].get();

  KJ_REQUIRE(array.size() >= offset + segmentSize,
             "Message ends prematurely in first segment.") {
    return;
  }

  segment0 = array.slice(offset, offset + segmentSize);
  offset += segmentSize;

  if (segmentCount > 1) {
    moreSegments = kj::heapArray<kj::ArrayPtr<const word>>(segmentCount - 1);

    for (uint i = 1; i < segmentCount; i++) {
      uint segmentSize = table[i + 1].get();

      KJ_REQUIRE(array.size() >= offset + segmentSize, "Message ends prematurely.") {
        moreSegments = nullptr;
        return;
      }

      moreSegments[i - 1] = array.slice(offset, offset + segmentSize);
      offset += segmentSize;
    }
  }
}

kj::ArrayPtr<const word> FlatArrayMessageReader::getSegment(uint id) {
  if (id == 0) {
    return segment0;
  } else if (id <= moreSegments.size()) {
    return moreSegments[id - 1];
  } else {
    return nullptr;
  }
}

kj::Array<word> messageToFlatArray(kj::ArrayPtr<const kj::ArrayPtr<const word>> segments) {
  KJ_REQUIRE(segments.size() > 0, "Tried to serialize uninitialized message.");

  size_t totalSize = segments.size() / 2 + 1;

  for (auto& segment: segments) {
    totalSize += segment.size();
  }

  kj::Array<word> result = kj::heapArray<word>(totalSize);

  _::WireValue<uint32_t>* table =
      reinterpret_cast<_::WireValue<uint32_t>*>(result.begin());

  // We write the segment count - 1 because this makes the first word zero for single-segment
  // messages, improving compression.  We don't bother doing this with segment sizes because
  // one-word segments are rare anyway.
  table[0].set(segments.size() - 1);

  for (uint i = 0; i < segments.size(); i++) {
    table[i + 1].set(segments[i].size());
  }

  if (segments.size() % 2 == 0) {
    // Set padding byte.
    table[segments.size() + 1].set(0);
  }

  word* dst = result.begin() + segments.size() / 2 + 1;

  for (auto& segment: segments) {
    memcpy(dst, segment.begin(), segment.size() * sizeof(word));
    dst += segment.size();
  }

  KJ_DASSERT(dst == result.end(), "Buffer overrun/underrun bug in code above.");

  return kj::mv(result);
}

void writeMessage(kj::OutputStream& output, kj::ArrayPtr<const kj::ArrayPtr<const word>> segments) {
  KJ_REQUIRE(segments.size() > 0, "Tried to serialize uninitialized message.");

  _::WireValue<uint32_t> table[(segments.size() + 2) & ~size_t(1)];

  // We write the segment count - 1 because this makes the first word zero for single-segment
  // messages, improving compression.  We don't bother doing this with segment sizes because
  // one-word segments are rare anyway.
  table[0].set(segments.size() - 1);
  for (uint i = 0; i < segments.size(); i++) {
    table[i + 1].set(segments[i].size());
  }
  if (segments.size() % 2 == 0) {
    // Set padding byte.
    table[segments.size() + 1].set(0);
  }

  KJ_STACK_ARRAY(kj::ArrayPtr<const byte>, pieces, segments.size() + 1, 4, 32);
  pieces[0] = kj::arrayPtr(reinterpret_cast<byte*>(table), sizeof(table));

  for (uint i = 0; i < segments.size(); i++) {
    pieces[i + 1] = kj::arrayPtr(reinterpret_cast<const byte*>(segments[i].begin()),
                                 reinterpret_cast<const byte*>(segments[i].end()));
  }

  output.write(pieces);
}

}  // namespace capnp
