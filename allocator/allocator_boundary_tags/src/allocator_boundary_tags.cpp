#include "../include/allocator_boundary_tags.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace
{
    constexpr size_t boundary_occupied_meta_size = sizeof(size_t) + 3 * sizeof(void *);

    struct boundary_block
    {
        size_t offset;
        size_t size;
        bool occupied;
    };

    struct boundary_state
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t space_size;
        std::vector<boundary_block> blocks;
    };

    std::unordered_map<void *, boundary_state> g_boundary_states;
    std::mutex g_boundary_states_mutex;

    boundary_state &state_for(void *trusted_memory)
    {
        return g_boundary_states.at(trusted_memory);
    }

    const boundary_state &state_for(const void *trusted_memory)
    {
        return g_boundary_states.at(const_cast<void *>(trusted_memory));
    }

    size_t required_boundary_block(size_t payload)
    {
        return std::max(payload, size_t{0}) + boundary_occupied_meta_size;
    }

    void merge_boundary_blocks(boundary_state &state)
    {
        if (state.blocks.empty())
        {
            return;
        }

        std::vector<boundary_block> merged;
        merged.reserve(state.blocks.size());
        for (const auto &block : state.blocks)
        {
            if (!merged.empty())
            {
                auto &last = merged.back();
                if (!last.occupied && !block.occupied && last.offset + last.size == block.offset)
                {
                    last.size += block.size;
                    continue;
                }
            }
            merged.push_back(block);
        }
        state.blocks = std::move(merged);
    }
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    auto it = g_boundary_states.find(_trusted_memory);
    if (it != g_boundary_states.end())
    {
        it->second.parent->deallocate(_trusted_memory, it->second.space_size, alignof(std::max_align_t));
        g_boundary_states.erase(it);
    }
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags &&other) noexcept:
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(
    allocator_boundary_tags &&other) noexcept
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


allocator_boundary_tags::allocator_boundary_tags(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < boundary_occupied_meta_size)
    {
        throw std::logic_error("allocator_boundary_tags requires more memory");
    }

    auto *resource = parent_allocator == nullptr ? std::pmr::get_default_resource() : parent_allocator;
    _trusted_memory = resource->allocate(space_size, alignof(std::max_align_t));

    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    g_boundary_states[_trusted_memory] = boundary_state{
        .parent = resource,
        .mode = allocate_fit_mode,
        .space_size = space_size,
        .blocks = { boundary_block{ .offset = 0, .size = space_size, .occupied = false } }
    };
}

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    auto &state = state_for(_trusted_memory);
    const size_t wanted = required_boundary_block(size);

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

    auto &block = state.blocks[selected];
    const size_t block_offset = block.offset;
    const size_t remainder = block.size - wanted;
    block.occupied = true;
    block.size = wanted;

    if (remainder >= boundary_occupied_meta_size)
    {
        state.blocks.insert(
            state.blocks.begin() + static_cast<ptrdiff_t>(selected + 1),
            boundary_block{ .offset = block_offset + wanted, .size = remainder, .occupied = false });
    }
    else
    {
        block.size += remainder;
    }

    return reinterpret_cast<unsigned char *>(_trusted_memory) + block_offset + boundary_occupied_meta_size;
}

void allocator_boundary_tags::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    auto *ptr = reinterpret_cast<unsigned char *>(at);

    if (ptr < base + boundary_occupied_meta_size || ptr >= base + state.space_size)
    {
        throw std::logic_error("allocator_boundary_tags deallocate out of range");
    }

    const size_t offset = static_cast<size_t>(ptr - base - boundary_occupied_meta_size);
    auto it = std::find_if(
        state.blocks.begin(),
        state.blocks.end(),
        [offset](const boundary_block &block)
        {
            return block.offset == offset;
        });

    if (it == state.blocks.end() || !it->occupied)
    {
        throw std::logic_error("allocator_boundary_tags deallocate invalid block");
    }

    it->occupied = false;
    merge_boundary_blocks(state);
}

inline void allocator_boundary_tags::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    state_for(_trusted_memory).mode = mode;
}


std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    return get_blocks_info_inner();
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    const auto &state = state_for(_trusted_memory);
    boundary_iterator it;
    it._trusted_memory = _trusted_memory;
    if (state.blocks.empty())
    {
        it._occupied_ptr = nullptr;
        it._occupied = false;
        return it;
    }

    it._occupied_ptr = reinterpret_cast<unsigned char *>(_trusted_memory) + state.blocks.front().offset;
    it._occupied = state.blocks.front().occupied;
    return it;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    boundary_iterator it;
    it._trusted_memory = _trusted_memory;
    it._occupied_ptr = nullptr;
    it._occupied = false;
    return it;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
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

allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &other):
    allocator_boundary_tags(state_for(other._trusted_memory).space_size, state_for(other._trusted_memory).parent, state_for(other._trusted_memory).mode)
{
    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    const auto &other_state = state_for(other._trusted_memory);
    auto &this_state = state_for(_trusted_memory);
    std::memcpy(_trusted_memory, other._trusted_memory, other_state.space_size);
    this_state.blocks = other_state.blocks;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(const allocator_boundary_tags &other)
{
    if (this == &other)
    {
        return *this;
    }

    auto copy(other);
    *this = std::move(copy);
    return *this;
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_boundary_tags *>(&other) != nullptr;
}

bool allocator_boundary_tags::boundary_iterator::operator==(
        const allocator_boundary_tags::boundary_iterator &other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr && _trusted_memory == other._trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
        const allocator_boundary_tags::boundary_iterator & other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr)
    {
        return *this;
    }

    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    const auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_occupied_ptr) - base);
    for (size_t i = 0; i < state.blocks.size(); ++i)
    {
        if (state.blocks[i].offset == offset)
        {
            if (i + 1 < state.blocks.size())
            {
                _occupied_ptr = base + state.blocks[i + 1].offset;
                _occupied = state.blocks[i + 1].occupied;
            }
            else
            {
                _occupied_ptr = nullptr;
                _occupied = false;
            }
            return *this;
        }
    }

    _occupied_ptr = nullptr;
    _occupied = false;
    return *this;
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_trusted_memory == nullptr)
    {
        return *this;
    }

    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    const auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);

    if (_occupied_ptr == nullptr)
    {
        if (!state.blocks.empty())
        {
            const auto &block = state.blocks.back();
            _occupied_ptr = base + block.offset;
            _occupied = block.occupied;
        }
        return *this;
    }

    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_occupied_ptr) - base);
    for (size_t i = 0; i < state.blocks.size(); ++i)
    {
        if (state.blocks[i].offset == offset)
        {
            if (i > 0)
            {
                _occupied_ptr = base + state.blocks[i - 1].offset;
                _occupied = state.blocks[i - 1].occupied;
            }
            return *this;
        }
    }

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
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> guard(g_boundary_states_mutex);
    const auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_occupied_ptr) - base);
    for (const auto &block : state.blocks)
    {
        if (block.offset == offset)
        {
            return block.size;
        }
    }
    return 0;
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
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
    _occupied(false),
    _trusted_memory(trusted)
{
}

void *allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}
