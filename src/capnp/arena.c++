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

#define CAPNP_PRIVATE
#include "arena.h"
#include "message.h"
#include "capability.h"
#include <kj/debug.h>
#include <kj/refcount.h>
#include <vector>
#include <string.h>
#include <stdio.h>

namespace capnp {
namespace _ {  // private

Arena::~Arena() noexcept(false) {}

void ReadLimiter::unread(WordCount64 amount) {
  // Be careful not to overflow here.  Since ReadLimiter has no thread-safety, it's possible that
  // the limit value was not updated correctly for one or more reads, and therefore unread() could
  // overflow it even if it is only unreading bytes that were actually read.
  uint64_t oldValue = limit;
  uint64_t newValue = oldValue + amount / WORDS;
  if (newValue > oldValue) {
    limit = newValue;
  }
}

// =======================================================================================

ReaderArena::ReaderArena(MessageReader* message)
    : message(message),
      readLimiter(message->getOptions().traversalLimitInWords * WORDS),
      segment0(this, SegmentId(0), message->getSegment(0), &readLimiter) {}

ReaderArena::~ReaderArena() noexcept(false) {}

SegmentReader* ReaderArena::tryGetSegment(SegmentId id) {
  if (id == SegmentId(0)) {
    if (segment0.getArray() == nullptr) {
      return nullptr;
    } else {
      return &segment0;
    }
  }

  auto lock = moreSegments.lockExclusive();

  SegmentMap* segments = nullptr;
  KJ_IF_MAYBE(s, *lock) {
    auto iter = s->get()->find(id.value);
    if (iter != s->get()->end()) {
      return iter->second;
    }
    segments = *s;
  }

  kj::ArrayPtr<const word> newSegment = message->getSegment(id.value);
  if (newSegment == nullptr) {
    return nullptr;
  }

  if (*lock == nullptr) {
    // OK, the segment exists, so allocate the map.
    auto s = kj::heap<SegmentMap>();
    segments = s;
    *lock = kj::mv(s);
  }

  auto segment = kj::heap<SegmentReader>(this, id, newSegment, &readLimiter);
  SegmentReader* result = segment;
  segments->insert(std::make_pair(id.value, mv(segment)));
  return result;
}

void ReaderArena::reportReadLimitReached() {
  KJ_FAIL_REQUIRE("Exceeded message traversal limit.  See capnp::ReaderOptions.") {
    return;
  }
}

kj::Maybe<kj::Own<ClientHook>> ReaderArena::extractCap(uint index) {
  if (index < capTable.size()) {
    return capTable[index].map([](kj::Own<ClientHook>& cap) { return cap->addRef(); });
  } else {
    return nullptr;
  }
}

// =======================================================================================

BuilderArena::BuilderArena(MessageBuilder* message)
    : message(message), segment0(nullptr, SegmentId(0), nullptr, nullptr) {}
BuilderArena::~BuilderArena() noexcept(false) {}

SegmentBuilder* BuilderArena::getSegment(SegmentId id) {
  // This method is allowed to fail if the segment ID is not valid.
  if (id == SegmentId(0)) {
    return &segment0;
  } else {
    KJ_IF_MAYBE(s, moreSegments) {
      KJ_REQUIRE(id.value - 1 < s->get()->builders.size(), "invalid segment id", id.value);
      return const_cast<SegmentBuilder*>(s->get()->builders[id.value - 1].get());
    } else {
      KJ_FAIL_REQUIRE("invalid segment id", id.value);
    }
  }
}

BuilderArena::AllocateResult BuilderArena::allocate(WordCount amount) {
  if (segment0.getArena() == nullptr) {
    // We're allocating the first segment.
    kj::ArrayPtr<word> ptr = message->allocateSegment(amount / WORDS);

    // Re-allocate segment0 in-place.  This is a bit of a hack, but we have not returned any
    // pointers to this segment yet, so it should be fine.
    kj::dtor(segment0);
    kj::ctor(segment0, this, SegmentId(0), ptr, &this->dummyLimiter);
    return AllocateResult { &segment0, segment0.allocate(amount) };
  } else {
    // Check if there is space in the first segment.
    word* attempt = segment0.allocate(amount);
    if (attempt != nullptr) {
      return AllocateResult { &segment0, attempt };
    }

    // Need to fall back to additional segments.

    MultiSegmentState* segmentState;
    KJ_IF_MAYBE(s, moreSegments) {
      // TODO(perf):  Check for available space in more than just the last segment.  We don't
      //   want this to be O(n), though, so we'll need to maintain some sort of table.  Complicating
      //   matters, we want SegmentBuilders::allocate() to be fast, so we can't update any such
      //   table when allocation actually happens.  Instead, we could have a priority queue based
      //   on the last-known available size, and then re-check the size when we pop segments off it
      //   and shove them to the back of the queue if they have become too small.

      attempt = s->get()->builders.back()->allocate(amount);
      if (attempt != nullptr) {
        return AllocateResult { s->get()->builders.back().get(), attempt };
      }
      segmentState = *s;
    } else {
      auto newSegmentState = kj::heap<MultiSegmentState>();
      segmentState = newSegmentState;
      moreSegments = kj::mv(newSegmentState);
    }

    kj::Own<SegmentBuilder> newBuilder = kj::heap<SegmentBuilder>(
        this, SegmentId(segmentState->builders.size() + 1),
        message->allocateSegment(amount / WORDS), &this->dummyLimiter);
    SegmentBuilder* result = newBuilder.get();
    segmentState->builders.add(kj::mv(newBuilder));

    // Keep forOutput the right size so that we don't have to re-allocate during
    // getSegmentsForOutput(), which callers might reasonably expect is a thread-safe method.
    segmentState->forOutput.resize(segmentState->builders.size() + 1);

    // Allocating from the new segment is guaranteed to succeed since we made it big enough.
    return AllocateResult { result, result->allocate(amount) };
  }
}

kj::ArrayPtr<const kj::ArrayPtr<const word>> BuilderArena::getSegmentsForOutput() {
  // Although this is a read-only method, we shouldn't need to lock a mutex here because if this
  // is called multiple times simultaneously, we should only be overwriting the array with the
  // exact same data.  If the number or size of segments is actually changing due to an activity
  // in another thread, then the caller has a problem regardless of locking here.

  KJ_IF_MAYBE(segmentState, moreSegments) {
    KJ_DASSERT(segmentState->get()->forOutput.size() == segmentState->get()->builders.size() + 1,
        "segmentState->forOutput wasn't resized correctly when the last builder was added.",
        segmentState->get()->forOutput.size(), segmentState->get()->builders.size());

    kj::ArrayPtr<kj::ArrayPtr<const word>> result(
        &segmentState->get()->forOutput[0], segmentState->get()->forOutput.size());
    uint i = 0;
    result[i++] = segment0.currentlyAllocated();
    for (auto& builder: segmentState->get()->builders) {
      result[i++] = builder->currentlyAllocated();
    }
    return result;
  } else {
    if (segment0.getArena() == nullptr) {
      // We haven't actually allocated any segments yet.
      return nullptr;
    } else {
      // We have only one segment so far.
      segment0ForOutput = segment0.currentlyAllocated();
      return kj::arrayPtr(&segment0ForOutput, 1);
    }
  }
}

SegmentReader* BuilderArena::tryGetSegment(SegmentId id) {
  if (id == SegmentId(0)) {
    if (segment0.getArena() == nullptr) {
      // We haven't allocated any segments yet.
      return nullptr;
    } else {
      return &segment0;
    }
  } else {
    KJ_IF_MAYBE(segmentState, moreSegments) {
      if (id.value <= segmentState->get()->builders.size()) {
        // TODO(cleanup):  Return a const SegmentReader and tediously constify all SegmentBuilder
        //   pointers throughout the codebase.
        return const_cast<SegmentReader*>(kj::implicitCast<const SegmentReader*>(
            segmentState->get()->builders[id.value - 1].get()));
      }
    }
    return nullptr;
  }
}

void BuilderArena::reportReadLimitReached() {
  KJ_FAIL_ASSERT("Read limit reached for BuilderArena, but it should have been unlimited.") {
    return;
  }
}

kj::Maybe<kj::Own<ClientHook>> BuilderArena::extractCap(uint index) {
  if (index < capTable.size()) {
    return capTable[index].map([](kj::Own<ClientHook>& cap) { return cap->addRef(); });
  } else {
    return nullptr;
  }
}

uint BuilderArena::injectCap(kj::Own<ClientHook>&& cap) {
  // TODO(perf):  Detect if the cap is already on the table and reuse the index?  Perhaps this
  //   doesn't happen enough to be worth the effort.
  uint result = capTable.size();
  capTable.add(kj::mv(cap));
  return result;
}

void BuilderArena::dropCap(uint index) {
  KJ_ASSERT(index < capTable.size(), "Invalid capability descriptor in message.") {
    return;
  }
  capTable[index] = nullptr;
}

}  // namespace _ (private)
}  // namespace capnp
