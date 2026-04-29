#include "../include/allocator_boundary_tags.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>

namespace
{
    constexpr size_t align_up(size_t value, size_t alignment) noexcept
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    struct boundary_block
    {
        size_t size;
        boundary_block *previous;
        boundary_block *next;
        bool occupied;
    };

    struct allocator_state
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t user_space_size;
        size_t allocated_size;
        boundary_block *first;
        std::mutex mutex;
    };

    static_assert(sizeof(boundary_block) == sizeof(size_t) + 3 * sizeof(void *));

    constexpr size_t state_offset = align_up(sizeof(allocator_state), alignof(std::max_align_t));
    constexpr size_t block_metadata_size = sizeof(boundary_block);

    allocator_state *state_of(void *trusted) noexcept
    {
        return static_cast<allocator_state *>(trusted);
    }

    const allocator_state *state_of(const void *trusted) noexcept
    {
        return static_cast<const allocator_state *>(trusted);
    }

    unsigned char *pool_begin(void *trusted) noexcept
    {
        return static_cast<unsigned char *>(trusted) + state_offset;
    }

    const unsigned char *pool_begin(const void *trusted) noexcept
    {
        return static_cast<const unsigned char *>(trusted) + state_offset;
    }

    size_t required_block_size(size_t payload) noexcept
    {
        return payload + block_metadata_size;
    }

    void split_if_possible(boundary_block *block, size_t wanted) noexcept
    {
        const size_t remainder = block->size - wanted;
        if (remainder < block_metadata_size)
        {
            return;
        }

        auto *created = reinterpret_cast<boundary_block *>(reinterpret_cast<unsigned char *>(block) + wanted);
        created->size = remainder;
        created->previous = block;
        created->next = block->next;
        created->occupied = false;
        if (created->next != nullptr)
        {
            created->next->previous = created;
        }

        block->size = wanted;
        block->next = created;
    }

    void merge_with_next(boundary_block *block) noexcept
    {
        auto *next = block->next;
        if (next == nullptr || next->occupied)
        {
            return;
        }

        block->size += next->size;
        block->next = next->next;
        if (block->next != nullptr)
        {
            block->next->previous = block;
        }
    }

    boundary_block *find_block_by_payload(allocator_state *state, void *payload)
    {
        const auto *begin = reinterpret_cast<const unsigned char *>(state->first);
        const auto *end = begin + state->user_space_size;
        auto *ptr = static_cast<unsigned char *>(payload);

        if (ptr < begin + block_metadata_size || ptr >= end)
        {
            throw std::logic_error("allocator_boundary_tags deallocate out of range");
        }

        for (auto *block = state->first; block != nullptr; block = block->next)
        {
            if (reinterpret_cast<unsigned char *>(block) + block_metadata_size == ptr)
            {
                return block;
            }
        }

        throw std::logic_error("allocator_boundary_tags deallocate invalid block");
    }

    void rebuild_links(allocator_state *state) noexcept
    {
        auto *cursor = reinterpret_cast<boundary_block *>(pool_begin(state));
        auto *end = pool_begin(state) + state->user_space_size;
        boundary_block *previous = nullptr;
        state->first = cursor;

        while (reinterpret_cast<unsigned char *>(cursor) < end)
        {
            cursor->previous = previous;
            auto *next_address = reinterpret_cast<unsigned char *>(cursor) + cursor->size;
            cursor->next = next_address < end ? reinterpret_cast<boundary_block *>(next_address) : nullptr;
            previous = cursor;
            cursor = cursor->next;
        }
    }
}

allocator_boundary_tags::allocator_boundary_tags(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < block_metadata_size)
    {
        throw std::logic_error("allocator_boundary_tags requires more memory");
    }

    auto *resource = parent_allocator == nullptr ? std::pmr::get_default_resource() : parent_allocator;
    const size_t allocated_size = state_offset + space_size;
    _trusted_memory = resource->allocate(allocated_size, alignof(std::max_align_t));

    try
    {
        auto *state = new (_trusted_memory) allocator_state{
            .parent = resource,
            .mode = allocate_fit_mode,
            .user_space_size = space_size,
            .allocated_size = allocated_size,
            .first = reinterpret_cast<boundary_block *>(pool_begin(_trusted_memory)),
            .mutex = {}
        };

        state->first->size = space_size;
        state->first->previous = nullptr;
        state->first->next = nullptr;
        state->first->occupied = false;
    }
    catch (...)
    {
        resource->deallocate(_trusted_memory, allocated_size, alignof(std::max_align_t));
        _trusted_memory = nullptr;
        throw;
    }
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto *state = state_of(_trusted_memory);
    auto *parent = state->parent;
    const size_t allocated_size = state->allocated_size;
    state->mutex.~mutex();
    parent->deallocate(_trusted_memory, allocated_size, alignof(std::max_align_t));
    _trusted_memory = nullptr;
}

allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags &&other) noexcept:
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(allocator_boundary_tags &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_boundary_tags();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &other)
{
    auto *other_state = state_of(other._trusted_memory);
    allocator_boundary_tags copy(other_state->user_space_size, other_state->parent, other_state->mode);

    {
        std::lock_guard<std::mutex> lock(other_state->mutex);
        auto *copy_state = state_of(copy._trusted_memory);
        std::memcpy(pool_begin(copy._trusted_memory), pool_begin(other._trusted_memory), other_state->user_space_size);
        copy_state->mode = other_state->mode;
        rebuild_links(copy_state);
    }

    _trusted_memory = copy._trusted_memory;
    copy._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(const allocator_boundary_tags &other)
{
    if (this == &other)
    {
        return *this;
    }

    allocator_boundary_tags copy(other);
    *this = std::move(copy);
    return *this;
}

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(size_t size)
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    const size_t wanted = required_block_size(size);
    boundary_block *selected = nullptr;

    for (auto *block = state->first; block != nullptr; block = block->next)
    {
        if (block->occupied || block->size < wanted)
        {
            continue;
        }

        if (selected == nullptr)
        {
            selected = block;
            if (state->mode == fit_mode::first_fit)
            {
                break;
            }
            continue;
        }

        if (state->mode == fit_mode::the_best_fit && block->size < selected->size)
        {
            selected = block;
        }
        else if (state->mode == fit_mode::the_worst_fit && block->size > selected->size)
        {
            selected = block;
        }
    }

    if (selected == nullptr)
    {
        throw std::bad_alloc();
    }

    split_if_possible(selected, wanted);
    selected->occupied = true;
    return reinterpret_cast<unsigned char *>(selected) + block_metadata_size;
}

void allocator_boundary_tags::do_deallocate_sm(void *at)
{
    if (at == nullptr)
    {
        return;
    }

    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    auto *block = find_block_by_payload(state, at);
    if (!block->occupied)
    {
        throw std::logic_error("allocator_boundary_tags deallocate invalid block");
    }

    block->occupied = false;
    merge_with_next(block);
    if (block->previous != nullptr && !block->previous->occupied)
    {
        merge_with_next(block->previous);
    }
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_boundary_tags *>(&other) != nullptr;
}

void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    const auto *state = state_of(_trusted_memory);
    std::vector<allocator_test_utils::block_info> result;
    for (auto *block = state->first; block != nullptr; block = block->next)
    {
        result.push_back({ .block_size = block->size, .is_block_occupied = block->occupied });
    }
    return result;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    return boundary_iterator(state->first);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator();
}

bool allocator_boundary_tags::boundary_iterator::operator==(const boundary_iterator &other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(const boundary_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    auto *block = static_cast<boundary_block *>(_occupied_ptr);
    block = block == nullptr ? nullptr : block->next;
    _occupied_ptr = block;
    _occupied = block != nullptr && block->occupied;
    return *this;
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    auto *block = static_cast<boundary_block *>(_occupied_ptr);
    block = block == nullptr ? nullptr : block->previous;
    _occupied_ptr = block;
    _occupied = block != nullptr && block->occupied;
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int)
{
    auto copy = *this;
    --(*this);
    return copy;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    auto *block = static_cast<boundary_block *>(_occupied_ptr);
    return block == nullptr ? 0 : block->size;
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    auto *block = static_cast<boundary_block *>(_occupied_ptr);
    return block != nullptr && block->occupied;
}

void *allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    return _occupied_ptr;
}

void *allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}

allocator_boundary_tags::boundary_iterator::boundary_iterator():
    _occupied_ptr(nullptr),
    _occupied(false),
    _trusted_memory(nullptr)
{
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void *trusted):
    _occupied_ptr(trusted),
    _occupied(trusted != nullptr && static_cast<boundary_block *>(trusted)->occupied),
    _trusted_memory(nullptr)
{
}
