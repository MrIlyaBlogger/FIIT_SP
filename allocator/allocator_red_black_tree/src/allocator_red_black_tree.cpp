#include "../include/allocator_red_black_tree.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace
{
    constexpr size_t rb_occupied_meta_size = 0;

    struct rb_block
    {
        size_t offset;
        size_t size;
        bool occupied;
    };

    struct rb_state
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t space_size;
        std::vector<rb_block> blocks;
    };

    std::unordered_map<void *, rb_state> g_rb_states;
    std::mutex g_rb_states_mutex;

    rb_state &state_for(void *trusted_memory)
    {
        return g_rb_states.at(trusted_memory);
    }

    const rb_state &state_for(const void *trusted_memory)
    {
        return g_rb_states.at(const_cast<void *>(trusted_memory));
    }

    size_t required_rb_block(size_t payload)
    {
        return std::max(payload, size_t{1});
    }

    void merge_rb_blocks(rb_state &state)
    {
        if (state.blocks.empty())
        {
            return;
        }

        std::vector<rb_block> merged;
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

allocator_red_black_tree::~allocator_red_black_tree()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    auto it = g_rb_states.find(_trusted_memory);
    if (it != g_rb_states.end())
    {
        it->second.parent->deallocate(_trusted_memory, it->second.space_size, alignof(std::max_align_t));
        g_rb_states.erase(it);
    }
}

allocator_red_black_tree::allocator_red_black_tree(
    allocator_red_black_tree &&other) noexcept:
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_red_black_tree &allocator_red_black_tree::operator=(
    allocator_red_black_tree &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_red_black_tree();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

allocator_red_black_tree::allocator_red_black_tree(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < rb_occupied_meta_size + 1)
    {
        throw std::logic_error("allocator_red_black_tree requires more memory");
    }

    auto *resource = parent_allocator == nullptr ? std::pmr::get_default_resource() : parent_allocator;
    _trusted_memory = resource->allocate(space_size, alignof(std::max_align_t));

    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    g_rb_states[_trusted_memory] = rb_state{
        .parent = resource,
        .mode = allocate_fit_mode,
        .space_size = space_size,
        .blocks = { rb_block{ .offset = 0, .size = space_size, .occupied = false } }
    };
}

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &other):
    allocator_red_black_tree(state_for(other._trusted_memory).space_size, state_for(other._trusted_memory).parent, state_for(other._trusted_memory).mode)
{
    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    const auto &other_state = state_for(other._trusted_memory);
    auto &this_state = state_for(_trusted_memory);
    std::memcpy(_trusted_memory, other._trusted_memory, other_state.space_size);
    this_state.blocks = other_state.blocks;
}

allocator_red_black_tree &allocator_red_black_tree::operator=(const allocator_red_black_tree &other)
{
    if (this == &other)
    {
        return *this;
    }

    auto copy(other);
    *this = std::move(copy);
    return *this;
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_red_black_tree *>(&other) != nullptr;
}

[[nodiscard]] void *allocator_red_black_tree::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    auto &state = state_for(_trusted_memory);
    const size_t wanted = required_rb_block(size);

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

    if (remainder > rb_occupied_meta_size)
    {
        state.blocks.insert(
            state.blocks.begin() + static_cast<ptrdiff_t>(selected + 1),
            rb_block{ .offset = block_offset + wanted, .size = remainder, .occupied = false });
    }
    else
    {
        block.size += remainder;
    }

    return reinterpret_cast<unsigned char *>(_trusted_memory) + block_offset + rb_occupied_meta_size;
}


void allocator_red_black_tree::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    auto *ptr = reinterpret_cast<unsigned char *>(at);

    if (ptr < base + rb_occupied_meta_size || ptr >= base + state.space_size)
    {
        throw std::logic_error("allocator_red_black_tree deallocate out of range");
    }

    const size_t offset = static_cast<size_t>(ptr - base - rb_occupied_meta_size);
    auto it = std::find_if(
        state.blocks.begin(),
        state.blocks.end(),
        [offset](const rb_block &block)
        {
            return block.offset == offset;
        });

    if (it == state.blocks.end() || !it->occupied)
    {
        throw std::logic_error("allocator_red_black_tree deallocate invalid block");
    }

    it->occupied = false;
    merge_rb_blocks(state);
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    state_for(_trusted_memory).mode = mode;
}


std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const
{
    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const
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


allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept
{
    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    const auto &state = state_for(_trusted_memory);
    rb_iterator it;
    it._trusted = _trusted_memory;
    if (state.blocks.empty())
    {
        it._block_ptr = nullptr;
        return it;
    }
    it._block_ptr = reinterpret_cast<unsigned char *>(_trusted_memory) + state.blocks.front().offset;
    return it;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept
{
    rb_iterator it;
    it._trusted = _trusted_memory;
    it._block_ptr = nullptr;
    return it;
}


bool allocator_red_black_tree::rb_iterator::operator==(const allocator_red_black_tree::rb_iterator &other) const noexcept
{
    return _block_ptr == other._block_ptr && _trusted == other._trusted;
}

bool allocator_red_black_tree::rb_iterator::operator!=(const allocator_red_black_tree::rb_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_red_black_tree::rb_iterator &allocator_red_black_tree::rb_iterator::operator++() & noexcept
{
    if (_block_ptr == nullptr || _trusted == nullptr)
    {
        return *this;
    }

    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    const auto &state = state_for(_trusted);
    auto *base = reinterpret_cast<unsigned char *>(_trusted);
    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_block_ptr) - base);
    for (size_t i = 0; i < state.blocks.size(); ++i)
    {
        if (state.blocks[i].offset == offset)
        {
            if (i + 1 < state.blocks.size())
            {
                _block_ptr = base + state.blocks[i + 1].offset;
            }
            else
            {
                _block_ptr = nullptr;
            }
            return *this;
        }
    }

    _block_ptr = nullptr;
    return *this;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::rb_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

size_t allocator_red_black_tree::rb_iterator::size() const noexcept
{
    if (_block_ptr == nullptr || _trusted == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    const auto &state = state_for(_trusted);
    auto *base = reinterpret_cast<unsigned char *>(_trusted);
    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_block_ptr) - base);
    for (const auto &block : state.blocks)
    {
        if (block.offset == offset)
        {
            return block.size;
        }
    }
    return 0;
}

void *allocator_red_black_tree::rb_iterator::operator*() const noexcept
{
    return _block_ptr;
}

allocator_red_black_tree::rb_iterator::rb_iterator():
    _block_ptr(nullptr),
    _trusted(nullptr)
{
}

allocator_red_black_tree::rb_iterator::rb_iterator(void *trusted):
    _block_ptr(trusted),
    _trusted(trusted)
{
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept
{
    if (_block_ptr == nullptr || _trusted == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> guard(g_rb_states_mutex);
    const auto &state = state_for(_trusted);
    auto *base = reinterpret_cast<unsigned char *>(_trusted);
    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_block_ptr) - base);
    for (const auto &block : state.blocks)
    {
        if (block.offset == offset)
        {
            return block.occupied;
        }
    }

    return false;
}
