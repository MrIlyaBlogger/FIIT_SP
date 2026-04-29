#include "../include/allocator_sorted_list.h"

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

    struct block_header
    {
        size_t size;
        block_header *next;
        block_header *prev;
        bool occupied;
    };

    struct allocator_state
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t user_space_size;
        size_t allocated_size;
        block_header *first;
        std::mutex mutex;
    };

    static_assert(sizeof(block_header) >= sizeof(void *) + sizeof(size_t));

    constexpr size_t state_offset = align_up(sizeof(allocator_state), alignof(std::max_align_t));

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
        return std::max(payload, size_t{1}) + sizeof(block_header);
    }

    block_header *split_if_possible(block_header *block, size_t wanted) noexcept
    {
        const size_t remainder = block->size - wanted;
        if (remainder <= sizeof(block_header))
        {
            return block;
        }

        auto *created = reinterpret_cast<block_header *>(reinterpret_cast<unsigned char *>(block) + wanted);
        created->size = remainder;
        created->occupied = false;
        created->prev = block;
        created->next = block->next;
        if (created->next != nullptr)
        {
            created->next->prev = created;
        }

        block->size = wanted;
        block->next = created;
        return block;
    }

    void merge_with_next(block_header *block) noexcept
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
            block->next->prev = block;
        }
    }

    block_header *find_block_by_payload(allocator_state *state, void *payload)
    {
        const auto *begin = reinterpret_cast<const unsigned char *>(state->first);
        const auto *end = begin + state->user_space_size;
        auto *ptr = static_cast<unsigned char *>(payload);

        if (ptr < begin + sizeof(block_header) || ptr >= end)
        {
            throw std::logic_error("allocator_sorted_list deallocate out of range");
        }

        auto *block = state->first;
        while (block != nullptr)
        {
            if (reinterpret_cast<unsigned char *>(block) + sizeof(block_header) == ptr)
            {
                return block;
            }
            block = block->next;
        }

        throw std::logic_error("allocator_sorted_list deallocate invalid block");
    }

    void rebuild_links(allocator_state *state) noexcept
    {
        auto *cursor = reinterpret_cast<block_header *>(pool_begin(state));
        auto *end = pool_begin(state) + state->user_space_size;
        block_header *previous = nullptr;
        state->first = cursor;

        while (reinterpret_cast<unsigned char *>(cursor) < end)
        {
            cursor->prev = previous;
            auto *next_address = reinterpret_cast<unsigned char *>(cursor) + cursor->size;
            cursor->next = next_address < end ? reinterpret_cast<block_header *>(next_address) : nullptr;
            previous = cursor;
            cursor = cursor->next;
        }
    }
}

allocator_sorted_list::allocator_sorted_list(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < sizeof(block_header) + 1)
    {
        throw std::logic_error("allocator_sorted_list requires more memory");
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
            .first = reinterpret_cast<block_header *>(pool_begin(_trusted_memory)),
            .mutex = {}
        };

        state->first->size = space_size;
        state->first->next = nullptr;
        state->first->prev = nullptr;
        state->first->occupied = false;
    }
    catch (...)
    {
        resource->deallocate(_trusted_memory, allocated_size, alignof(std::max_align_t));
        _trusted_memory = nullptr;
        throw;
    }
}

allocator_sorted_list::~allocator_sorted_list()
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

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list &&other) noexcept:
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(allocator_sorted_list &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_sorted_list();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)
{
    auto *other_state = state_of(other._trusted_memory);
    allocator_sorted_list copy(other_state->user_space_size, other_state->parent, other_state->mode);

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

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{
    if (this == &other)
    {
        return *this;
    }

    allocator_sorted_list copy(other);
    *this = std::move(copy);
    return *this;
}

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(size_t size)
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);

    const size_t wanted = required_block_size(size);
    block_header *selected = nullptr;

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
    return reinterpret_cast<unsigned char *>(selected) + sizeof(block_header);
}

void allocator_sorted_list::do_deallocate_sm(void *at)
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
        throw std::logic_error("allocator_sorted_list deallocate invalid block");
    }

    block->occupied = false;
    merge_with_next(block);
    if (block->prev != nullptr && !block->prev->occupied)
    {
        merge_with_next(block->prev);
    }
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_sorted_list *>(&other) != nullptr;
}

void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    const auto *state = state_of(_trusted_memory);
    std::vector<allocator_test_utils::block_info> result;
    for (auto *block = state->first; block != nullptr; block = block->next)
    {
        result.push_back({ .block_size = block->size, .is_block_occupied = block->occupied });
    }
    return result;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    for (auto *block = state->first; block != nullptr; block = block->next)
    {
        if (!block->occupied)
        {
            return sorted_free_iterator(block);
        }
    }
    return sorted_free_iterator();
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator();
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    return sorted_iterator(state->first);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    return sorted_iterator();
}

bool allocator_sorted_list::sorted_free_iterator::operator==(const sorted_free_iterator &other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(const sorted_free_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator &allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    auto *block = static_cast<block_header *>(_free_ptr);
    while (block != nullptr)
    {
        block = block->next;
        if (block != nullptr && !block->occupied)
        {
            _free_ptr = block;
            return *this;
        }
    }
    _free_ptr = nullptr;
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    auto *block = static_cast<block_header *>(_free_ptr);
    return block == nullptr ? 0 : block->size;
}

void *allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return _free_ptr;
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator():
    _free_ptr(nullptr)
{
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void *trusted):
    _free_ptr(trusted)
{
}

bool allocator_sorted_list::sorted_iterator::operator==(const sorted_iterator &other) const noexcept
{
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const sorted_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    auto *block = static_cast<block_header *>(_current_ptr);
    _current_ptr = block == nullptr ? nullptr : block->next;
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    auto *block = static_cast<block_header *>(_current_ptr);
    return block == nullptr ? 0 : block->size;
}

void *allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return _current_ptr;
}

allocator_sorted_list::sorted_iterator::sorted_iterator():
    _free_ptr(nullptr),
    _current_ptr(nullptr),
    _trusted_memory(nullptr)
{
}

allocator_sorted_list::sorted_iterator::sorted_iterator(void *trusted):
    _free_ptr(nullptr),
    _current_ptr(trusted),
    _trusted_memory(nullptr)
{
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    auto *block = static_cast<block_header *>(_current_ptr);
    return block != nullptr && block->occupied;
}
