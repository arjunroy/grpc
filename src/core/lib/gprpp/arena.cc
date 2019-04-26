/*
 *
 * Copyright 2017 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#include "src/core/lib/gprpp/arena.h"

#include <string.h>
#include <new>

#include <grpc/support/alloc.h>
#include <grpc/support/atm.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>

#include "src/core/lib/gpr/alloc.h"
#include "src/core/lib/gprpp/memory.h"

namespace grpc_core {

template <size_t alignment>
static void* _aligned_alloc(size_t size) {
#if defined(GPR_LINUX)
  if (alignment == GPR_CACHELINE_SIZE) {
    size = GPR_ROUND_UP_TO_CACHELINE_SIZE(size);
  } else {
    size = GPR_ROUND_UP_TO_ALIGNMENT_SIZE(size);
  }
  return aligned_alloc(alignment, size);
#else
  return gpr_malloc_aligned(size, alignment);
#endif
}

static void _aligned_free(void* ptr) {
#if defined(GPR_LINUX)
  free(ptr);
#else
  gpr_free_aligned(ptr);
#endif
}

Arena::~Arena() {
  Zone* z = last_zone_;
  while (z) {
    Zone* prev_z = z->prev;
    z->~Zone();
    _aligned_free(z);
    z = prev_z;
  }
}

Arena* Arena::Create(size_t initial_size) {
  static constexpr size_t base_size =
      GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(Arena));
  initial_size = GPR_ROUND_UP_TO_ALIGNMENT_SIZE(initial_size);
  size_t alloc_size = base_size + initial_size;
  static constexpr size_t alignment =
      (GPR_CACHELINE_SIZE > GPR_MAX_ALIGNMENT &&
       GPR_CACHELINE_SIZE % GPR_MAX_ALIGNMENT == 0)
          ? GPR_CACHELINE_SIZE
          : GPR_MAX_ALIGNMENT;
  void* storage = _aligned_alloc<alignment>(alloc_size);
  GPR_ASSERT(storage != nullptr);
  return new (storage) Arena(initial_size);
}

size_t Arena::Destroy() {
  size_t size = total_used_.Load(MemoryOrder::RELAXED);
  this->~Arena();
  _aligned_free(this);
  return size;
}

void* Arena::AllocZone(size_t size) {
  // If the allocation isn't able to end in the initial zone, create a new
  // zone for this allocation, and any unused space in the initial zone is
  // wasted. This overflowing and wasting is uncommon because of our arena
  // sizing hysteresis (that is, most calls should have a large enough initial
  // zone and will not need to grow the arena).
  static constexpr size_t zone_base_size =
      GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(Zone));
  size_t alloc_size = zone_base_size + size;
  void* storage = _aligned_alloc<GPR_MAX_ALIGNMENT>(alloc_size);
  GPR_ASSERT(storage != nullptr);
  Zone* z = new (storage) Zone();
  {
    gpr_spinlock_lock(&arena_growth_spinlock_);
    z->prev = last_zone_;
    last_zone_ = z;
    gpr_spinlock_unlock(&arena_growth_spinlock_);
  }
  return reinterpret_cast<char*>(z) + zone_base_size;
}

}  // namespace grpc_core
