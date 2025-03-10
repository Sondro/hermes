/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#ifndef HERMES_VM_ARRAYSTORAGE_H
#define HERMES_VM_ARRAYSTORAGE_H

#include "hermes/VM/HermesValue-inline.h"
#include "hermes/VM/Metadata.h"
#include "hermes/VM/Runtime.h"

#include "llvm/Support/TrailingObjects.h"

namespace hermes {
namespace vm {

/// A GC-managed resizable vector of values. It is used for storage of property
/// values in objects and also indexed property values in arrays. It supports
/// resizing on both ends which is necessary for the simplest implementation of
/// JavaScript arrays (using a base offset and length).
class ArrayStorage final
    : public VariableSizeRuntimeCell,
      private llvm::TrailingObjects<ArrayStorage, GCHermesValue> {
  friend TrailingObjects;
  friend void ArrayStorageBuildMeta(const GCCell *cell, Metadata::Builder &mb);

 public:
  using size_type = uint32_t;
  using iterator = GCHermesValue *;

  static VTable vt;

  /// Gets the amount of memory used by this object for a given \p capacity.
  static constexpr uint32_t allocationSize(size_type capacity) {
    return totalSizeToAlloc<GCHermesValue>(capacity);
  }

  /// \return The maximum number of elements we can fit in a single array in the
  /// current GC.
  static constexpr size_type maxElements() {
    return (GC::maxAllocationSize() - allocationSize(0)) /
        sizeof(GCHermesValue);
  }

  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::ArrayStorageKind;
  }

  /// Create a new instance with specified capacity.
  static CallResult<HermesValue> create(Runtime *runtime, size_type capacity) {
    if (LLVM_UNLIKELY(capacity > maxElements())) {
      return throwExcessiveCapacityError(runtime, capacity);
    }
    void *mem = runtime->alloc</*fixedSize*/ false>(allocationSize(capacity));
    return GCHermesValue::encodeObjectValue(
        new (mem) ArrayStorage(runtime, capacity));
  }

  /// Create a new long-lived instance with specified capacity.
  static CallResult<HermesValue> createLongLived(
      Runtime *runtime,
      size_type capacity) {
    if (LLVM_UNLIKELY(capacity > maxElements())) {
      return throwExcessiveCapacityError(runtime, capacity);
    }
    void *mem = runtime->allocLongLived(allocationSize(capacity));
    return GCHermesValue::encodeObjectValue(
        new (mem) ArrayStorage(runtime, capacity));
  }

  /// Create a new instance with specified capacity and size.
  /// Requires that \p size <= \p capacity.
  static CallResult<HermesValue>
  create(Runtime *runtime, size_type capacity, size_type size) {
    auto arrRes = create(runtime, capacity);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    ArrayStorage::resizeWithinCapacity(
        createPseudoHandle(vmcast<ArrayStorage>(*arrRes)), runtime, size);
    return arrRes;
  }

  /// \return a pointer to the underlying data storage.
  GCHermesValue *data() {
    return getTrailingObjects<GCHermesValue>();
  }
  const GCHermesValue *data() const {
    return getTrailingObjects<GCHermesValue>();
  }

  /// This enum is not needed here but is used for compatibility with
  /// SegmentedArray. It is intended to indicate that we know beforehand that
  /// an element is in the "inline storage". All storage here is "inline".
  enum class Inline { No, Yes };

  /// \return a reference to the element at index \p index
  template <Inline inl = Inline::No>
  GCHermesValue &at(size_type index) {
    assert(index < size_ && "index out of range");
    return data()[index];
  }

  size_type capacity() const {
    return capacity_;
  }
  size_type size() const {
    return size_;
  }

  iterator begin() {
    return data();
  }
  iterator end() {
    return data() + size_;
  }

  /// Append the given element to the end (increasing size by 1).
  static ExecutionStatus push_back(
      MutableHandle<ArrayStorage> &selfHandle,
      Runtime *runtime,
      Handle<> value) {
    auto *self = selfHandle.get();
    if (LLVM_LIKELY(self->size_ < self->capacity_)) {
      self->data()[self->size_++].set(value.get(), &runtime->getHeap());
      return ExecutionStatus::RETURNED;
    }
    return pushBackSlowPath(selfHandle, runtime, value);
  }

  /// Pop the last element off the array and return it.
  HermesValue pop_back() {
    assert(size_ > 0 && "Can't pop from empty ArrayStorage");
    return data()[--size_];
  }

  /// Ensure that the capacity of the array is at least \p capacity,
  /// reallocating if needed.
  static ExecutionStatus ensureCapacity(
      MutableHandle<ArrayStorage> &selfHandle,
      Runtime *runtime,
      size_type capacity);

  /// Change the size of the storage to \p newSize. This can increase the size
  /// (in which case the new elements will be initialized to empty), or decrease
  /// the size.
  static ExecutionStatus resize(
      MutableHandle<ArrayStorage> &selfHandle,
      Runtime *runtime,
      size_type newSize) {
    return shift(selfHandle, runtime, 0, 0, newSize);
  }

  /// The same as resize, but add elements to the left instead of the right.
  ///
  /// In the case where the capacity is sufficient to hold the \p newSize,
  /// every existing element is copied rightward, a linear time procedure.
  /// If the capacity is not sufficient, then the performance will be the same
  /// as \c resize.
  static ExecutionStatus resizeLeft(
      MutableHandle<ArrayStorage> &selfHandle,
      Runtime *runtime,
      size_type newSize) {
    return shift(selfHandle, runtime, 0, newSize - selfHandle->size_, newSize);
  }

  /// Set the size to a value <= the capacity. This is a special
  /// case of resize() but has a simpler interface since we know that it doesn't
  /// need to reallocate.
  static void resizeWithinCapacity(
      PseudoHandle<ArrayStorage> self,
      Runtime *,
      size_type newSize) {
    assert(
        newSize <= self->capacity_ &&
        "newSize must be <= capacity in resizeWithinCapacity()");
    // If enlarging, clear the new elements.
    if (newSize > self->size_) {
      GCHermesValue::fill(
          self->data() + self->size_,
          self->data() + newSize,
          HermesValue::encodeEmptyValue());
    }
    self->size_ = newSize;
  }

 private:
  /// The capacity is the maximum number of elements this array can ever
  /// contain. The capacity is constant after creation, with the exception of
  /// shrinking during a GC compaction. In order to increase the capacity, a new
  /// ArrayStorage must be created.
  size_type capacity_;
  size_type size_{0};

  ArrayStorage() = delete;
  ArrayStorage(const ArrayStorage &) = delete;
  void operator=(const ArrayStorage &) = delete;
  ~ArrayStorage() = delete;

  ArrayStorage(Runtime *runtime, size_type capacity)
      : VariableSizeRuntimeCell(
            &runtime->getHeap(),
            &vt,
            allocationSize(capacity)),
        capacity_(capacity) {}

  /// Throws a RangeError with a descriptive message describing the attempted
  /// capacity allocated, and the max that is allowed.
  /// \returns ExecutionStatus::EXCEPTION always.
  static ExecutionStatus throwExcessiveCapacityError(
      Runtime *runtime,
      size_type capacity);

  /// Append the given element to the end when the capacity has been exhausted
  /// and a reallocation is needed.
  static ExecutionStatus pushBackSlowPath(
      MutableHandle<ArrayStorage> &selfHandle,
      Runtime *runtime,
      Handle<> value);

  /// Shrinks \p self during GC compaction, so that it's capacity is equal to
  /// its size.
  /// \return the size the object will have when compaction is complete.
  static gcheapsize_t _trimSizeCallback(const GCCell *self);
  static void _trimCallback(GCCell *self);

  /// Reallocate to a larger storage capacity of the storage and copy the
  /// specified portion of the data to the new storage. The length of the data
  /// to be copied is
  ///   length = min(size - fromFirst, toLast - toFirst).
  /// "length" number of elements are copied from "fromFirst" to "toFirst".
  static ExecutionStatus reallocateToLarger(
      MutableHandle<ArrayStorage> &selfHandle,
      Runtime *runtime,
      size_type capacity,
      size_type fromFirst,
      size_type toFirst,
      size_type toLast);

// Mangling scheme used by MSVC encode public/private into the name.
// As a result, vanilla "ifdef public" trick leads to link errors.
#if defined(UNIT_TEST) || defined(_MSC_VER)
 public:
#endif
  /// This is a flexible function which can be used to extend the array by
  /// creating or removing elements in front or in the back. New elements are
  /// initialized to empty. Intuitively it shifts a specified number of elements
  /// to a new position and clears the rest. More precisely, it can be described
  /// as follows:
  /// 1. Resize the storage to contain `toLast` elements.
  /// 2. Copy the elements `[fromFirst..min(fromFirst+size, toLast-toFirst))` to
  ///       position 'toFirst'.
  /// 3. Set all elements before `toFirst` and after the last copied element to
  ///   "empty".
  static ExecutionStatus shift(
      MutableHandle<ArrayStorage> &selfHandle,
      Runtime *runtime,
      size_type fromFirst,
      size_type toFirst,
      size_type toLast);
};

static_assert(
    ArrayStorage::allocationSize(ArrayStorage::maxElements()) <=
        GC::maxAllocationSize(),
    "maxElements() is too big");

static_assert(
    GC::maxAllocationSize() -
            ArrayStorage::allocationSize(ArrayStorage::maxElements()) <
        HeapAlign,
    "maxElements() is too small");

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_ARRAYSTORAGE_H
