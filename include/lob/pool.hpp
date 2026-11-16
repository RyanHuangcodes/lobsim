#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LOB_POOL_ASAN 1
#endif
#endif
#if !defined(LOB_POOL_ASAN) && defined(__SANITIZE_ADDRESS__)
#define LOB_POOL_ASAN 1
#endif

#if defined(LOB_POOL_ASAN)
#include <sanitizer/asan_interface.h>
#define LOB_POOL_POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define LOB_POOL_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define LOB_POOL_POISON(p, n) ((void)(p), (void)(n))
#define LOB_POOL_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

namespace lob {

// Chunked object pool with stable addresses (SPEC 3.5). Memory comes in chunks of
// ChunkSize slots that are never moved or freed until the pool dies; released slots go to
// a free list and are reused (most recently released first).
//
// The free list is a separate vector of slot addresses, not links stored inside the
// slots: under AddressSanitizer a released slot is poisoned, so nothing may read it until
// create() unpoisons it again, and a dangling T* into a released slot is reported by the
// sanitizer instead of silently reading recycled memory. The free vector's capacity is
// reserved to the pool's total capacity whenever a chunk is added, so destroy() never
// allocates.
//
// T must be trivially destructible: the pool does not track which slots are live, so it
// cannot run destructors when it dies.
template <class T, std::size_t ChunkSize = 4096>
class ObjectPool {
  static_assert(ChunkSize > 0);
  static_assert(std::is_trivially_destructible_v<T>, "ObjectPool never runs destructors at pool death");

 public:
  ObjectPool() = default;
  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;

  ~ObjectPool() {
    for (const auto& chunk : chunks_) LOB_POOL_UNPOISON(chunk.get(), ChunkSize * sizeof(Slot));
  }

  // Constructs a T in a free slot (growing by one chunk if there is none).
  template <class... Args>
  T* create(Args&&... args) {
    if (free_.empty()) grow();
    void* mem = free_.back();
    free_.pop_back();
    LOB_POOL_UNPOISON(mem, sizeof(Slot));
    T* obj = ::new (mem) T(std::forward<Args>(args)...);
    ++live_;
    return obj;
  }

  // Returns `obj` (which must come from this pool and be live) to the free list.
  void destroy(T* obj) noexcept {
    obj->~T();
    void* mem = static_cast<void*>(obj);
    free_.push_back(mem);  // capacity reserved in grow(): never reallocates
    LOB_POOL_POISON(mem, sizeof(Slot));
    --live_;
  }

  std::size_t live() const noexcept { return live_; }
  std::size_t capacity() const noexcept { return chunks_.size() * ChunkSize; }
  std::size_t chunks() const noexcept { return chunks_.size(); }

 private:
  struct alignas(T) Slot {
    std::byte bytes[sizeof(T)];
  };

  void grow() {
    std::unique_ptr<Slot[]> chunk(new Slot[ChunkSize]);
    Slot* base = chunk.get();
    chunks_.push_back(std::move(chunk));
    free_.reserve(capacity());
    // Pushed in reverse so the chunk is handed out from its first slot upwards.
    for (std::size_t i = ChunkSize; i-- > 0;) free_.push_back(static_cast<void*>(base + i));
    LOB_POOL_POISON(base, ChunkSize * sizeof(Slot));
  }

  std::vector<std::unique_ptr<Slot[]>> chunks_;
  std::vector<void*> free_;
  std::size_t live_{0};
};

}  // namespace lob
