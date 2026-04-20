#include "../include/allocator_global_heap.h"

allocator_global_heap::allocator_global_heap()
{
}

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(
    size_t size)
{
    return ::operator new(size);
}

void allocator_global_heap::do_deallocate_sm(
    void *at)
{
    ::operator delete(at);
}

allocator_global_heap::~allocator_global_heap()
{
}

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other)
{
    (void)other;
}

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other)
{
    (void)other;
    return *this;
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_global_heap *>(&other) != nullptr;
}

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept
{
    (void)other;
}

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept
{
    (void)other;
    return *this;
}
