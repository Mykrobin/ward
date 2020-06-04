#pragma once

#include "log2.hh"
#include "percpu.hh"
#include "atomic_util.hh"

#include <atomic>
#include <typeinfo>
#include <memory>

template<class T>
struct vptr48 {
  typedef u64 __inttype;
  typedef T __ptrtype;
  __inttype _a;

  T ptr() const {
    u64 i = iptr();
    if (i & (1ULL << 47))
      i += 0xffffULL << 48;
    return (T) i;
  }
  u64 iptr() const { return _a & 0xffffffffffffULL; }
  u16 v() const { return _a >> 48; }

  vptr48(T p, u16 v) : _a((((u64)v)<<48) | (((u64)p) & 0xffffffffffffULL)) {}
  vptr48(u64 a) : _a(a) {}
};

template<class VPTR>
class versioned {
 private:
  std::atomic<typename VPTR::__inttype> _a;

 public:
  VPTR load() { return VPTR(_a.load()); }
  bool compare_exchange(const VPTR &expected, typename VPTR::__ptrtype desired) {
    VPTR n(desired, expected.v() + 1);
    return cmpxch(&_a, expected._a, n._a);
  }
};

enum {
  slab_perf,
  slab_type_max
};

// std allocator

template<class T>
class allocator_base
{
public:
  typedef std::size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef T* pointer;
  typedef const T* const_pointer;
  typedef T& reference;
  typedef const T& const_reference;
  typedef T value_type;

  pointer
  address(reference x) const noexcept
  {
    return std::addressof(x);
  }

  const_pointer
  address(const_reference x) const noexcept
  {
    return std::addressof(x);
  }

  template<class U, class... Args>
  void
  construct(U* p, Args&&... args)
  {
    ::new((void *)p) U(std::forward<Args>(args)...);
  }

  template <class U>
  void
  destroy(U* p)
  {
    p->~U();
  }
};

// Standard allocator that uses the kernel page allocator.  This
// satisfies both the standard Allocator requirement as well as the
// ZAllocator requirement.
template<class T>
class kalloc_allocator : public allocator_base<T>
{
public:
  template <class U> struct rebind { typedef kalloc_allocator<U> other; };

  kalloc_allocator() = default;
  kalloc_allocator(const kalloc_allocator&) = default;
  template<class U> kalloc_allocator(const kalloc_allocator<U>&) noexcept { }

  T*
  allocate(std::size_t n, const void *hint = 0)
  {
    if (n * sizeof(T) < PGSIZE)
      panic("%s cannot allocate %zu bytes", __PRETTY_FUNCTION__, n * sizeof(T));
    return (T*)kalloc(typeid(T).name(), round_up_to_pow2(n * sizeof(T)));
  }

  void
  deallocate(T* p, std::size_t n)
  {
    if (n * sizeof(T) < PGSIZE)
      panic("%s cannot deallocate %zu bytes", __PRETTY_FUNCTION__, n * sizeof(T));
    kfree(p, round_up_to_pow2(n * sizeof(T)));
  }

  // ZAllocator methods

  T*
  default_allocate()
  {
    if (sizeof(T) != PGSIZE)
      panic("%s cannot allocate %zu bytes", __PRETTY_FUNCTION__, sizeof(T));

    if (std::is_trivially_default_constructible<T>::value) {
      // A trivial default constructor will zero-initialize
      // everything, so we can short-circuit this by allocating a zero
      // page.
      return (T*)zalloc(typeid(T).name());
    }

    // Fall back to usual allocation and default construction
    T *p = allocate(1);
    try {
      // Unqualified lookup doesn't find declarations in dependent
      // bases.  Hence "this->".
      this->construct(p);
    } catch (...) {
      deallocate(p, 1);
      throw;
    }
    return p;
  }
};

// Standard allocator that uses a vmap's qalloc functions to allocate memory.
template<class T>
class qalloc_allocator : public allocator_base<T>
{
public:
  template <class U> struct rebind { typedef qalloc_allocator<U> other; };

  qalloc_allocator(vmap* vmap) : vmap_(vmap) {}
  qalloc_allocator(const qalloc_allocator&) = default;
  template<class U> qalloc_allocator(const qalloc_allocator<U>& o) noexcept : vmap_(o.vmap_) { }

  T*
  allocate(std::size_t n, const void *hint = 0)
  {
    if (n * sizeof(T) != PGSIZE)
      panic("%s cannot allocate %zu bytes", __PRETTY_FUNCTION__, n * sizeof(T));
    return (T*)qalloc(vmap_, typeid(T).name());
  }

  void
  deallocate(T* p, std::size_t n)
  {
    if (n * sizeof(T) != PGSIZE)
      panic("%s cannot deallocate %zu bytes", __PRETTY_FUNCTION__,
            n * sizeof(T));
    qfree(vmap_, p);
  }

  std::size_t
  max_size() const noexcept
  {
    return PGSIZE;
  }

  // ZAllocator methods

  T*
  default_allocate()
  {
    if (sizeof(T) != PGSIZE)
      panic("%s cannot allocate %zu bytes", __PRETTY_FUNCTION__, sizeof(T));

    if (std::is_trivially_default_constructible<T>::value) {
      // A trivial default constructor will zero-initialize
      // everything, so we can short-circuit this by allocating a zero
      // page.
      return (T*)qalloc(vmap_, typeid(T).name());
    }

    // Fall back to usual allocation and default construction
    T *p = allocate(1);
    try {
      // Unqualified lookup doesn't find declarations in dependent
      // bases.  Hence "this->".
      this->construct(p);
    } catch (...) {
      deallocate(p, 1);
      throw;
    }
    return p;
  }

private:
  vmap* vmap_;
};

// Standard allocator that uses the kernel's public page allocator.  This
// satisfies both the standard Allocator requirement as well as the ZAllocator
// requirement.
template<class T>
class palloc_allocator : public allocator_base<T>
{
public:
  template <class U> struct rebind { typedef palloc_allocator<U> other; };

  palloc_allocator() = default;
  palloc_allocator(const palloc_allocator&) = default;
  template<class U> palloc_allocator(const palloc_allocator<U>&) noexcept { }

  T*
  allocate(std::size_t n, const void *hint = 0)
  {
    if (n * sizeof(T) != PGSIZE)
      panic("%s cannot allocate %zu bytes", __PRETTY_FUNCTION__, n * sizeof(T));
    return (T*)palloc(typeid(T).name());
  }

  void
  deallocate(T* p, std::size_t n)
  {
    if (n * sizeof(T) != PGSIZE)
      panic("%s cannot deallocate %zu bytes", __PRETTY_FUNCTION__,
            n * sizeof(T));
    pfree(p);
  }

  // ZAllocator methods

  T*
  default_allocate()
  {
    T *p = allocate(1);
    try {
      // Unqualified lookup doesn't find declarations in dependent
      // bases.  Hence "this->".
      this->construct(p);
    } catch (...) {
      deallocate(p, 1);
      throw;
    }
    return p;
  }
};

template<class T>
class kmalloc_allocator : public allocator_base<T>
{
public:
  template <class U> struct rebind { typedef kmalloc_allocator<U> other; };

  kmalloc_allocator() = default;
  kmalloc_allocator(const kmalloc_allocator&) = default;
  template<class U> kmalloc_allocator(const kmalloc_allocator<U>&) noexcept { }

  T*
  allocate(std::size_t n, const void *hint = 0)
  {
    return (T*)kmalloc(round_up_to_pow2(n * sizeof(T)), typeid(T).name());
  }

  void
  deallocate(T* p, std::size_t n)
  {
    kmfree(p, round_up_to_pow2(n * sizeof(T)));
  }

  bool operator==(kmalloc_allocator<T>) { return true; }
  bool operator!=(kmalloc_allocator<T>) { return false; }
};
