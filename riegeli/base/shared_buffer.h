// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef RIEGELI_BASE_SHARED_BUFFER_H_
#define RIEGELI_BASE_SHARED_BUFFER_H_

#include <stddef.h>

#include <atomic>
#include <utility>

#include "absl/strings/cord.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"
#include "riegeli/base/memory.h"

namespace riegeli {

// Dynamically allocated byte buffer.
//
// Like `Buffer`, but ownership of the data can be shared.
class SharedBuffer {
 public:
  SharedBuffer() noexcept {}

  // Ensures at least `min_capacity` of space.
  explicit SharedBuffer(size_t min_capacity);

  SharedBuffer(const SharedBuffer& that) noexcept;
  SharedBuffer& operator=(const SharedBuffer& that) noexcept;

  // The source `SharedBuffer` is left deallocated.
  SharedBuffer(SharedBuffer&& that) noexcept;
  SharedBuffer& operator=(SharedBuffer&& that) noexcept;

  ~SharedBuffer();

  // Ensures at least `min_capacity` of space and unique ownership of the data.
  // Existing contents are lost.
  void Reset(size_t min_capacity);

  // Returns true if this `SharedBuffer` is the only owner of the data.
  bool has_unique_owner() const;

  // Returns the mutable data pointer.
  //
  // Precondition: `has_unique_owner()`.
  char* mutable_data() const;

  // Returns the const data pointer.
  const char* const_data() const;

  // Returns the usable data size. It can be greater than the requested size.
  size_t capacity() const;

  // Returns an opaque pointer, which represents a share of ownership of the
  // data; an active share keeps the data alive. The returned pointer must be
  // deleted using `DeleteShared()`.
  //
  // If the returned pointer is `nullptr`, it allowed but not required to call
  // `DeleteShared()`.
  void* Share() const;

  // Deletes the pointer obtained by `Share()`.
  //
  // Does nothing if `ptr == nullptr`.
  static void DeleteShared(void* ptr);

  // Converts `*this` to `absl::Cord` by sharing the ownership of the data.
  // `substr` must be contained in `*this`.
  absl::Cord ToCord(absl::string_view substr) const;

 private:
  struct Payload {
    void Ref();
    void Unref();

    std::atomic<size_t> ref_count{1};
    // Usable size of the data starting at `allocated_begin`, i.e. excluding the
    // header.
    size_t capacity;
    // Beginning of data (actual allocated size is larger).
    char allocated_begin[1];
  };

  void AllocateInternal(size_t min_capacity);

  Payload* payload_ = nullptr;
  // Invariant: if `data_ == nullptr` then `capacity_ == 0`
};

// Implementation details follow.

inline void SharedBuffer::Payload::Ref() {
  ref_count.fetch_add(1, std::memory_order_relaxed);
}

inline void SharedBuffer::Payload::Unref() {
  // Optimization: avoid an expensive atomic read-modify-write operation if the
  // reference count is 1.
  if (ref_count.load(std::memory_order_acquire) == 1 ||
      ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    DeleteAligned<Payload>(this, offsetof(Payload, allocated_begin) + capacity);
  }
}

inline SharedBuffer::SharedBuffer(size_t min_capacity) {
  AllocateInternal(min_capacity);
}

inline SharedBuffer::SharedBuffer(const SharedBuffer& that) noexcept
    : payload_(that.payload_) {
  if (payload_ != nullptr) payload_->Ref();
}

inline SharedBuffer& SharedBuffer::operator=(
    const SharedBuffer& that) noexcept {
  Payload* const payload = that.payload_;
  if (payload != nullptr) payload->Ref();
  if (payload_ != nullptr) payload_->Unref();
  payload_ = payload;
  return *this;
}

inline SharedBuffer::SharedBuffer(SharedBuffer&& that) noexcept
    : payload_(std::exchange(that.payload_, nullptr)) {}

inline SharedBuffer& SharedBuffer::operator=(SharedBuffer&& that) noexcept {
  // Exchange `that.data_` early to support self-assignment.
  Payload* const payload = std::exchange(that.payload_, nullptr);
  if (payload_ != nullptr) payload_->Unref();
  payload_ = payload;
  return *this;
}

inline SharedBuffer::~SharedBuffer() {
  if (payload_ != nullptr) payload_->Unref();
}

inline void SharedBuffer::Reset(size_t min_capacity) {
  if (payload_ != nullptr) {
    if (has_unique_owner() && payload_->capacity >= min_capacity) return;
    min_capacity = UnsignedMax(
        min_capacity, SaturatingAdd(payload_->capacity, payload_->capacity));
    payload_->Unref();
  }
  AllocateInternal(min_capacity);
}

inline bool SharedBuffer::has_unique_owner() const {
  if (payload_ == nullptr) return true;
  return payload_->ref_count.load(std::memory_order_acquire) == 1;
}

inline char* SharedBuffer::mutable_data() const {
  RIEGELI_ASSERT(has_unique_owner())
      << "Failed precondition of SharedBuffer::mutable_data(): "
         "ownership is shared";
  if (payload_ == nullptr) return nullptr;
  return payload_->allocated_begin;
}

inline const char* SharedBuffer::const_data() const {
  if (payload_ == nullptr) return nullptr;
  return payload_->allocated_begin;
}

inline size_t SharedBuffer::capacity() const {
  if (payload_ == nullptr) return 0;
  return payload_->capacity;
}

inline void SharedBuffer::AllocateInternal(size_t min_capacity) {
  size_t raw_capacity;
  payload_ = SizeReturningNewAligned<Payload>(
      offsetof(Payload, allocated_begin) + min_capacity, &raw_capacity);
  payload_->capacity = raw_capacity - offsetof(Payload, allocated_begin);
}

inline void* SharedBuffer::Share() const {
  if (payload_ == nullptr) return nullptr;
  payload_->Ref();
  return payload_;
}

inline void SharedBuffer::DeleteShared(void* ptr) {
  if (ptr != nullptr) static_cast<Payload*>(ptr)->Unref();
}

}  // namespace riegeli

#endif  // RIEGELI_BASE_SHARED_BUFFER_H_
