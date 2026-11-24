#include "me/alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::uint64_t> g_allocations{0};

void* counted_alloc(std::size_t size) {
  void* p = std::malloc(size != 0 ? size : 1);
  if (p == nullptr) throw std::bad_alloc();
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  return p;
}

void* counted_alloc(std::size_t size, std::align_val_t align) {
  void* p = nullptr;
  std::size_t alignment = static_cast<std::size_t>(align);
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  if (posix_memalign(&p, alignment, size != 0 ? size : 1) != 0) throw std::bad_alloc();
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  return p;
}

}  // namespace

namespace me {

std::uint64_t alloc_count() noexcept {
  return g_allocations.load(std::memory_order_relaxed);
}

}  // namespace me

void* operator new(std::size_t size) {
  return counted_alloc(size);
}
void* operator new[](std::size_t size) {
  return counted_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t align) {
  return counted_alloc(size, align);
}
void* operator new[](std::size_t size, std::align_val_t align) {
  return counted_alloc(size, align);
}

void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
