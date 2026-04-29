#include "../include/allocator_buddies_system.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace
{
    constexpr size_t align_up(size_t value, size_t alignment) noexcept
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    struct buddy_header
    {
        unsigned char order;
        bool occupied;
    };

    struct allocator_state
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t user_space_size;
        size_t allocated_size;
        unsigned char max_order;
        std::mutex mutex;
    };

    constexpr size_t state_offset = align_up(sizeof(allocator_state), alignof(std::max_align_t));
    constexpr size_t occupied_metadata_size = sizeof(buddy_header);

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

    bool is_power_of_two(size_t value) noexcept
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    size_t round_up_power_of_two(size_t value)
    {
        if (value == 0)
        {
            return 1;
        }

        size_t power = 1;
        while (power < value)
        {
            if (power > (std::numeric_limits<size_t>::max() >> 1U))
            {
                throw std::bad_alloc();
            }
            power <<= 1U;
        }
        return power;
    }

    unsigned char order_of(size_t value)
    {
        unsigned char order = 0;
        while ((size_t{1} << order) < value)
        {
            ++order;
        }
        return order;
    }

    size_t size_of(const buddy_header *block) noexcept
    {
        return size_t{1} << block->order;
    }

    size_t minimum_block_size() noexcept
    {
        return size_t{1} << __detail::nearest_greater_k_of_2(sizeof(allocator_dbg_helper::block_pointer_t) + 1);
    }

    size_t required_block_size(size_t payload)
    {
        return round_up_power_of_two(std::max(payload + occupied_metadata_size, minimum_block_size()));
    }

    buddy_header *next_block(buddy_header *block) noexcept
    {
        return reinterpret_cast<buddy_header *>(reinterpret_cast<unsigned char *>(block) + size_of(block));
    }

    const buddy_header *next_block(const buddy_header *block) noexcept
    {
        return reinterpret_cast<const buddy_header *>(reinterpret_cast<const unsigned char *>(block) + size_of(block));
    }

    buddy_header *find_block_by_payload(allocator_state *state, void *payload)
    {
        auto *begin = pool_begin(state);
        auto *end = begin + state->user_space_size;
        auto *ptr = static_cast<unsigned char *>(payload);

        if (ptr < begin + occupied_metadata_size || ptr >= end)
        {
            throw std::logic_error("allocator_buddies_system deallocate out of range");
        }

        for (auto *block = reinterpret_cast<buddy_header *>(begin);
             reinterpret_cast<unsigned char *>(block) < end;
             block = next_block(block))
        {
            if (reinterpret_cast<unsigned char *>(block) + occupied_metadata_size == ptr)
            {
                return block;
            }
        }

        throw std::logic_error("allocator_buddies_system deallocate invalid block");
    }

    buddy_header *find_buddy(allocator_state *state, buddy_header *block) noexcept
    {
        auto *begin = pool_begin(state);
        const size_t block_size = size_of(block);
        const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(block) - begin);
        const size_t buddy_offset = offset ^ block_size;

        for (auto *candidate = reinterpret_cast<buddy_header *>(begin);
             reinterpret_cast<unsigned char *>(candidate) < begin + state->user_space_size;
             candidate = next_block(candidate))
        {
            if (static_cast<size_t>(reinterpret_cast<unsigned char *>(candidate) - begin) == buddy_offset)
            {
                return candidate;
            }
        }
        return nullptr;
    }
}

allocator_buddies_system::allocator_buddies_system(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    const size_t rounded_space = round_up_power_of_two(space_size);
    if (space_size < minimum_block_size() || !is_power_of_two(rounded_space))
    {
        throw std::logic_error("allocator_buddies_system requires a larger power-of-two block");
    }

    auto *resource = parent_allocator == nullptr ? std::pmr::get_default_resource() : parent_allocator;
    const size_t allocated_size = state_offset + rounded_space;
    _trusted_memory = resource->allocate(allocated_size, alignof(std::max_align_t));

    try
    {
        auto *state = new (_trusted_memory) allocator_state{
            .parent = resource,
            .mode = allocate_fit_mode,
            .user_space_size = rounded_space,
            .allocated_size = allocated_size,
            .max_order = order_of(rounded_space),
            .mutex = {}
        };

        auto *first = reinterpret_cast<buddy_header *>(pool_begin(_trusted_memory));
        first->order = state->max_order;
        first->occupied = false;
    }
    catch (...)
    {
        resource->deallocate(_trusted_memory, allocated_size, alignof(std::max_align_t));
        _trusted_memory = nullptr;
        throw;
    }
}

allocator_buddies_system::~allocator_buddies_system()
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

allocator_buddies_system::allocator_buddies_system(allocator_buddies_system &&other) noexcept:
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_buddies_system &allocator_buddies_system::operator=(allocator_buddies_system &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_buddies_system();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

allocator_buddies_system::allocator_buddies_system(const allocator_buddies_system &other)
{
    auto *other_state = state_of(other._trusted_memory);
    allocator_buddies_system copy(other_state->user_space_size, other_state->parent, other_state->mode);

    {
        std::lock_guard<std::mutex> lock(other_state->mutex);
        auto *copy_state = state_of(copy._trusted_memory);
        std::memcpy(pool_begin(copy._trusted_memory), pool_begin(other._trusted_memory), other_state->user_space_size);
        copy_state->mode = other_state->mode;
    }

    _trusted_memory = copy._trusted_memory;
    copy._trusted_memory = nullptr;
}

allocator_buddies_system &allocator_buddies_system::operator=(const allocator_buddies_system &other)
{
    if (this == &other)
    {
        return *this;
    }

    allocator_buddies_system copy(other);
    *this = std::move(copy);
    return *this;
}

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(size_t size)
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    const size_t wanted = required_block_size(size);
    buddy_header *selected = nullptr;
    auto *end = pool_begin(_trusted_memory) + state->user_space_size;

    for (auto *block = reinterpret_cast<buddy_header *>(pool_begin(_trusted_memory));
         reinterpret_cast<unsigned char *>(block) < end;
         block = next_block(block))
    {
        if (block->occupied || size_of(block) < wanted)
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

        if (state->mode == fit_mode::the_best_fit && size_of(block) < size_of(selected))
        {
            selected = block;
        }
        else if (state->mode == fit_mode::the_worst_fit && size_of(block) > size_of(selected))
        {
            selected = block;
        }
    }

    if (selected == nullptr)
    {
        throw std::bad_alloc();
    }

    while (size_of(selected) / 2 >= wanted && size_of(selected) / 2 >= minimum_block_size())
    {
        --selected->order;
        auto *right = reinterpret_cast<buddy_header *>(reinterpret_cast<unsigned char *>(selected) + size_of(selected));
        right->order = selected->order;
        right->occupied = false;
    }

    selected->occupied = true;
    return reinterpret_cast<unsigned char *>(selected) + occupied_metadata_size;
}

void allocator_buddies_system::do_deallocate_sm(void *at)
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
        throw std::logic_error("allocator_buddies_system deallocate invalid block");
    }

    block->occupied = false;
    while (block->order < state->max_order)
    {
        auto *buddy = find_buddy(state, block);
        if (buddy == nullptr || buddy->occupied || buddy->order != block->order)
        {
            break;
        }

        if (buddy < block)
        {
            block = buddy;
        }
        ++block->order;
        block->occupied = false;
    }
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_buddies_system *>(&other) != nullptr;
}

void allocator_buddies_system::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    const auto *state = state_of(_trusted_memory);
    std::vector<allocator_test_utils::block_info> result;
    const auto *end = pool_begin(_trusted_memory) + state->user_space_size;

    for (auto *block = reinterpret_cast<const buddy_header *>(pool_begin(_trusted_memory));
         reinterpret_cast<const unsigned char *>(block) < end;
         block = next_block(block))
    {
        result.push_back({ .block_size = size_of(block), .is_block_occupied = block->occupied });
    }
    return result;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    return buddy_iterator(pool_begin(_trusted_memory));
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    return buddy_iterator();
}

bool allocator_buddies_system::buddy_iterator::operator==(const buddy_iterator &other) const noexcept
{
    return _block == other._block;
}

bool allocator_buddies_system::buddy_iterator::operator!=(const buddy_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_buddies_system::buddy_iterator &allocator_buddies_system::buddy_iterator::operator++() & noexcept
{
    auto *block = static_cast<buddy_header *>(_block);
    _block = block == nullptr ? nullptr : reinterpret_cast<unsigned char *>(block) + size_of(block);
    return *this;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

size_t allocator_buddies_system::buddy_iterator::size() const noexcept
{
    auto *block = static_cast<buddy_header *>(_block);
    return block == nullptr ? 0 : size_of(block);
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept
{
    auto *block = static_cast<buddy_header *>(_block);
    return block != nullptr && block->occupied;
}

void *allocator_buddies_system::buddy_iterator::operator*() const noexcept
{
    return _block;
}

allocator_buddies_system::buddy_iterator::buddy_iterator():
    _block(nullptr)
{
}

allocator_buddies_system::buddy_iterator::buddy_iterator(void *start):
    _block(start)
{
}
