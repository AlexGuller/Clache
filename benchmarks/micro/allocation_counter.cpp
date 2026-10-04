#include "allocation_counter.h"

#include <cstdlib>
#include <new>

namespace {
thread_local bool counting = false;
thread_local std::uint64_t allocations = 0;

void* allocate(std::size_t size, std::size_t alignment = 0) {
  for (;;) {
    void* result = nullptr;
    if (alignment == 0) {
      result = std::malloc(size == 0 ? 1 : size);
    } else if (posix_memalign(&result, alignment, size == 0 ? 1 : size) != 0) {
      result = nullptr;
    }
    if (result != nullptr) {
      if (counting) ++allocations;
      return result;
    }
    const auto handler = std::get_new_handler();
    if (handler == nullptr) throw std::bad_alloc();
    handler();
  }
}
}  // namespace

namespace clache::bench {
void begin_allocations() noexcept {
  allocations = 0;
  counting = true;
}
std::uint64_t end_allocations() noexcept {
  counting = false;
  return allocations;
}
}  // namespace clache::bench

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, tag);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return allocate(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return ::operator new(size, alignment, tag);
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(ptr);
}
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(ptr);
}
