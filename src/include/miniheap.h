// -*- mode: c++ -*-
// Copyright 2016 University of Massachusetts, Amherst

#ifndef MESH_MINIHEAP_H
#define MESH_MINIHEAP_H

#include <mutex>
#include <random>

#include "heaplayers.h"
#include "internal.h"
#include "rng/mwc.h"

using namespace HL;

uint32_t seed() {
  // single random_device to seed the random number generators in MiniHeaps
  static std::random_device rd;
  static std::mutex rdLock;

  std::lock_guard<std::mutex> lock(rdLock);

  uint32_t seed = 0;
  while (seed == 0) {
    seed = rd();
  }
  return seed;
}

template <typename SuperHeap,
          typename InternalAlloc,
          size_t PageSize = 4096,
          size_t MinObjectSize = 16,
          size_t MaxObjectSize = 2048,
          size_t MinAvailablePages = 4,
          size_t SpanSize = 128UL * 1024UL, // 128Kb spans
          unsigned int FullNumerator = 3,
          unsigned int FullDenominator = 4>
class MiniHeapBase : public SuperHeap {
public:
  enum { Alignment = (int)MinObjectSize };
  static const size_t span_size = SpanSize;

  MiniHeapBase(size_t objectSize)
    : SuperHeap(), _objectSize(objectSize), _maxCount(), _fullCount(), _inUseCount(),
        _rng(seed(), seed()), _bitmap(), _done(false) {

    d_assert(_inUseCount == 0);
    _span = SuperHeap::malloc(SpanSize);
    if (!_span)
      abort();

    d_assert(_inUseCount == 0);

    constexpr auto heapPages = SpanSize / PageSize;
    _maxCount = SpanSize / objectSize;
    _fullCount = FullNumerator * _maxCount / FullDenominator;

    _bitmap.reserve(_maxCount);

    debug("MiniHeap(%zu): reserving %zu objects on %zu pages (%u/%u full: %zu/%d inUse: %zu)\t%p-%p\n",
          objectSize, _maxCount, heapPages, FullNumerator, FullDenominator, _fullCount,
          this->isFull(), _inUseCount, _span, reinterpret_cast<uintptr_t>(_span)+SpanSize);
  }

  inline void *malloc(size_t sz) {
    d_assert(!_done);
    d_assert_msg(sz == _objectSize, "sz: %zu _objectSize: %zu", sz, _objectSize);

    // should never have been called
    if (isFull())
      abort();

    while (true) {
      //auto off = _rng.next() % _maxCount;
      size_t off = seed() % _maxCount;

      if (_bitmap.tryToSet(off)) {
        auto ptr = reinterpret_cast<void *>((uintptr_t)_span + off * _objectSize);
        _inUseCount++;
        auto ptrval = reinterpret_cast<uintptr_t>(ptr);
        auto spanStart = reinterpret_cast<uintptr_t>(_span);
        d_assert_msg(ptrval+sz <= spanStart+SpanSize,
                     "OOB alloc? sz:%zu (%p-%p) ptr:%p rand:%zu count:%zu osize:%zu\n", sz, _span, spanStart+SpanSize, ptrval, random, _maxCount, _objectSize);
        return ptr;
      }
    }
  }

  inline void free(void *ptr) {
    d_assert(getSize(ptr) == _objectSize);
    d_assert(!_done);

    auto span = reinterpret_cast<uintptr_t>(_span);
    auto ptrval = reinterpret_cast<uintptr_t>(ptr);

    d_assert(span <= ptrval);
    auto off = (ptrval - span)/_objectSize;
    d_assert(off < _maxCount);

    if (_bitmap.isSet(off)) {
      _bitmap.reset(off);
      _inUseCount--;
      if (_done) {
        debug("MiniHeap(%p): FREE %4d/%4d (%f) -- size %zu", this, _inUseCount, _maxCount, (double)_inUseCount/_maxCount, _objectSize);
      }
      if (_inUseCount == 0 && _done) {
        SuperHeap::free(_span);
        // _span = (void *)0xdeadbeef;
        debug("FIXME: free MiniHeap metadata");
      }
    } else {
      debug("MiniHeap(%p): caught double free of %p?", this, ptrval);
    }

    d_assert(_inUseCount >= 0);
  }

  inline size_t getSize(void *ptr) {
    auto span = reinterpret_cast<uintptr_t>(_span);
    auto ptrval = reinterpret_cast<uintptr_t>(ptr);
    d_assert_msg(span <= ptrval && ptrval < span + SpanSize, "span(%p) <= %p < %p", span, ptrval, span + SpanSize);

    return _objectSize;
  }

  inline bool isFull() {
    return _inUseCount >= _fullCount;
  }

  inline uintptr_t getSpanStart() {
    return reinterpret_cast<uintptr_t>(_span);
  }

  inline void setDone() {
    debug("MiniHeap(%p): DONE %4d/%4d (%f) -- size %zu", this, _inUseCount, _maxCount, (double)_inUseCount/_maxCount, _objectSize);
    _done = true;
  }

  void *_span;
  size_t _objectSize;
  size_t _maxCount;
  size_t _fullCount;
  size_t _inUseCount;
  MWC _rng;
  BitMap<InternalAlloc> _bitmap;
  bool _done;
};

#endif  // MESH_MINIHEAP_H