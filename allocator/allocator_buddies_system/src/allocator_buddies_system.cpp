#include "../include/allocator_buddies_system.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace
{
    struct buddy_block_metadata
    {
        bool occupied : 1;
        unsigned char size : 7;
    };

    constexpr size_t buddy_occupied_block_metadata_size = sizeof(buddy_block_metadata) + sizeof(void *);
    constexpr size_t buddy_min_k = __detail::nearest_greater_k_of_2(buddy_occupied_block_metadata_size);

    struct buddy_block
    {
        size_t offset;
        size_t size;
        bool occupied;
    };

    struct buddy_state
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t space_size;
        std::vector<buddy_block> blocks;
    };

    std::unordered_map<void *, buddy_state> g_buddy_states;
    std::mutex g_buddy_states_mutex;

    buddy_state &state_for(void *trusted_memory)
    {
        return g_buddy_states.at(trusted_memory);
    }

    const buddy_state &state_for(const void *trusted_memory)
    {
        return g_buddy_states.at(const_cast<void *>(trusted_memory));
    }

    size_t round_up_power_of_two(size_t value)
    {
        size_t power = 1;
        while (power < value)
        {
            power <<= 1U;
        }
        return power;
    }

    size_t minimum_buddy_block_size()
    {
        return size_t{1} << buddy_min_k;
    }

    size_t required_buddy_block(size_t payload)
    {
        return round_up_power_of_two(std::max(payload + buddy_occupied_block_metadata_size, minimum_buddy_block_size()));
    }
}

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    auto it = g_buddy_states.find(_trusted_memory);
    if (it != g_buddy_states.end())
    {
        it->second.parent->deallocate(_trusted_memory, it->second.space_size, alignof(std::max_align_t));
        g_buddy_states.erase(it);
    }
}

allocator_buddies_system::allocator_buddies_system(
    allocator_buddies_system &&other) noexcept:
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_buddies_system &allocator_buddies_system::operator=(
    allocator_buddies_system &&other) noexcept
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

allocator_buddies_system::allocator_buddies_system(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    const size_t rounded_space = round_up_power_of_two(space_size);
    if (rounded_space < minimum_buddy_block_size())
    {
        throw std::logic_error("allocator_buddies_system requires a larger power-of-two block");
    }

    auto *resource = parent_allocator == nullptr ? std::pmr::get_default_resource() : parent_allocator;
    _trusted_memory = resource->allocate(rounded_space, alignof(std::max_align_t));

    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    g_buddy_states[_trusted_memory] = buddy_state{
        .parent = resource,
        .mode = allocate_fit_mode,
        .space_size = rounded_space,
        .blocks = { buddy_block{ .offset = 0, .size = rounded_space, .occupied = false } }
    };
}

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    auto &state = state_for(_trusted_memory);
    const size_t wanted = required_buddy_block(size);

    size_t selected = state.blocks.size();
    for (size_t i = 0; i < state.blocks.size(); ++i)
    {
        const auto &block = state.blocks[i];
        if (block.occupied || block.size < wanted)
        {
            continue;
        }

        if (selected == state.blocks.size())
        {
            selected = i;
            if (state.mode == fit_mode::first_fit)
            {
                break;
            }
            continue;
        }

        if (state.mode == fit_mode::the_best_fit && block.size < state.blocks[selected].size)
        {
            selected = i;
        }
        if (state.mode == fit_mode::the_worst_fit && block.size > state.blocks[selected].size)
        {
            selected = i;
        }
    }

    if (selected == state.blocks.size())
    {
        throw std::bad_alloc();
    }

    while (state.blocks[selected].size / 2 >= wanted && state.blocks[selected].size / 2 >= minimum_buddy_block_size())
    {
        const auto current = state.blocks[selected];
        const size_t half = current.size / 2;
        state.blocks[selected] = buddy_block{ .offset = current.offset, .size = half, .occupied = false };
        state.blocks.insert(
            state.blocks.begin() + static_cast<ptrdiff_t>(selected + 1),
            buddy_block{ .offset = current.offset + half, .size = half, .occupied = false });
    }

    state.blocks[selected].occupied = true;
    return reinterpret_cast<unsigned char *>(_trusted_memory) + state.blocks[selected].offset + buddy_occupied_block_metadata_size;
}

void allocator_buddies_system::do_deallocate_sm(void *at)
{
    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    auto *ptr = reinterpret_cast<unsigned char *>(at);

    if (ptr < base + buddy_occupied_block_metadata_size || ptr >= base + state.space_size)
    {
        throw std::logic_error("allocator_buddies_system deallocate out of range");
    }

    const size_t offset = static_cast<size_t>(ptr - base - buddy_occupied_block_metadata_size);
    auto it = std::find_if(
        state.blocks.begin(),
        state.blocks.end(),
        [offset](const buddy_block &block)
        {
            return block.offset == offset;
        });

    if (it == state.blocks.end() || !it->occupied)
    {
        throw std::logic_error("allocator_buddies_system deallocate invalid block");
    }

    it->occupied = false;

    bool merged = true;
    while (merged)
    {
        merged = false;
        std::sort(
            state.blocks.begin(),
            state.blocks.end(),
            [](const buddy_block &lhs, const buddy_block &rhs)
            {
                return lhs.offset < rhs.offset;
            });

        for (size_t i = 0; i + 1 < state.blocks.size(); ++i)
        {
            auto &left = state.blocks[i];
            auto &right = state.blocks[i + 1];
            if (left.occupied || right.occupied || left.size != right.size)
            {
                continue;
            }

            if ((left.offset ^ left.size) == right.offset && left.offset + left.size == right.offset)
            {
                left.size *= 2;
                state.blocks.erase(state.blocks.begin() + static_cast<ptrdiff_t>(i + 1));
                merged = true;
                break;
            }
        }
    }
}

allocator_buddies_system::allocator_buddies_system(const allocator_buddies_system &other):
    allocator_buddies_system(state_for(other._trusted_memory).space_size, state_for(other._trusted_memory).parent, state_for(other._trusted_memory).mode)
{
    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    const auto &other_state = state_for(other._trusted_memory);
    auto &this_state = state_for(_trusted_memory);
    std::memcpy(_trusted_memory, other._trusted_memory, other_state.space_size);
    this_state.blocks = other_state.blocks;
}

allocator_buddies_system &allocator_buddies_system::operator=(const allocator_buddies_system &other)
{
    if (this == &other)
    {
        return *this;
    }

    auto copy(other);
    *this = std::move(copy);
    return *this;
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_buddies_system *>(&other) != nullptr;
}

inline void allocator_buddies_system::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    state_for(_trusted_memory).mode = mode;
}


std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    const auto &state = state_for(_trusted_memory);
    std::vector<allocator_test_utils::block_info> result;
    result.reserve(state.blocks.size());
    for (const auto &block : state.blocks)
    {
        result.push_back({ .block_size = block.size, .is_block_occupied = block.occupied });
    }
    return result;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    const auto &state = state_for(_trusted_memory);
    buddy_iterator it;
    if (state.blocks.empty())
    {
        it._block = nullptr;
        return it;
    }
    it._block = reinterpret_cast<unsigned char *>(_trusted_memory) + state.blocks.front().offset;
    return it;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    return buddy_iterator();
}

bool allocator_buddies_system::buddy_iterator::operator==(const allocator_buddies_system::buddy_iterator &other) const noexcept
{
    return _block == other._block;
}

bool allocator_buddies_system::buddy_iterator::operator!=(const allocator_buddies_system::buddy_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_buddies_system::buddy_iterator &allocator_buddies_system::buddy_iterator::operator++() & noexcept
{
    if (_block == nullptr)
    {
        return *this;
    }

    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    for (const auto &[trusted, state] : g_buddy_states)
    {
        auto *base = reinterpret_cast<unsigned char *>(trusted);
        auto *current = reinterpret_cast<unsigned char *>(_block);
        if (current < base || current >= base + state.space_size)
        {
            continue;
        }

        const size_t offset = static_cast<size_t>(current - base);
        for (size_t i = 0; i < state.blocks.size(); ++i)
        {
            if (state.blocks[i].offset == offset)
            {
                if (i + 1 < state.blocks.size())
                {
                    _block = base + state.blocks[i + 1].offset;
                }
                else
                {
                    _block = nullptr;
                }
                return *this;
            }
        }
    }

    _block = nullptr;
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
    if (_block == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    for (const auto &[trusted, state] : g_buddy_states)
    {
        auto *base = reinterpret_cast<unsigned char *>(trusted);
        auto *current = reinterpret_cast<unsigned char *>(_block);
        if (current < base || current >= base + state.space_size)
        {
            continue;
        }
        const size_t offset = static_cast<size_t>(current - base);
        for (const auto &block : state.blocks)
        {
            if (block.offset == offset)
            {
                return block.size;
            }
        }
    }
    return 0;
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept
{
    if (_block == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> guard(g_buddy_states_mutex);
    for (const auto &[trusted, state] : g_buddy_states)
    {
        auto *base = reinterpret_cast<unsigned char *>(trusted);
        auto *current = reinterpret_cast<unsigned char *>(_block);
        if (current < base || current >= base + state.space_size)
        {
            continue;
        }
        const size_t offset = static_cast<size_t>(current - base);
        for (const auto &block : state.blocks)
        {
            if (block.offset == offset)
            {
                return block.occupied;
            }
        }
    }
    return false;
}

void *allocator_buddies_system::buddy_iterator::operator*() const noexcept
{
    return _block;
}

allocator_buddies_system::buddy_iterator::buddy_iterator(void *start):
    _block(start)
{
}

allocator_buddies_system::buddy_iterator::buddy_iterator():
    _block(nullptr)
{
}
