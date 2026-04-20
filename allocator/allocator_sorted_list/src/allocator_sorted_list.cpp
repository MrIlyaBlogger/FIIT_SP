#include "../include/allocator_sorted_list.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace
{
    constexpr size_t sorted_block_metadata_size = sizeof(void *) + sizeof(size_t);

    struct sorted_block
    {
        size_t offset;
        size_t size;
        bool occupied;
    };

    struct sorted_state
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t space_size;
        std::vector<sorted_block> blocks;
    };

    std::unordered_map<void *, sorted_state> g_sorted_states;
    std::mutex g_sorted_states_mutex;

    sorted_state &state_for(void *trusted_memory)
    {
        return g_sorted_states.at(trusted_memory);
    }

    const sorted_state &state_for(const void *trusted_memory)
    {
        return g_sorted_states.at(const_cast<void *>(trusted_memory));
    }

    size_t required_block_size(size_t payload)
    {
        return std::max(payload, size_t{1}) + sorted_block_metadata_size;
    }

    void merge_sorted_blocks(sorted_state &state)
    {
        if (state.blocks.empty())
        {
            return;
        }

        std::vector<sorted_block> merged;
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

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    auto it = g_sorted_states.find(_trusted_memory);
    if (it != g_sorted_states.end())
    {
        it->second.parent->deallocate(_trusted_memory, it->second.space_size, alignof(std::max_align_t));
        g_sorted_states.erase(it);
    }
}

allocator_sorted_list::allocator_sorted_list(
    allocator_sorted_list &&other) noexcept:
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(
    allocator_sorted_list &&other) noexcept
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

allocator_sorted_list::allocator_sorted_list(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < sorted_block_metadata_size + 1)
    {
        throw std::logic_error("allocator_sorted_list requires more memory");
    }

    auto *resource = parent_allocator == nullptr ? std::pmr::get_default_resource() : parent_allocator;
    _trusted_memory = resource->allocate(space_size, alignof(std::max_align_t));

    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    g_sorted_states[_trusted_memory] = sorted_state{
        .parent = resource,
        .mode = allocate_fit_mode,
        .space_size = space_size,
        .blocks = { sorted_block{ .offset = 0, .size = space_size, .occupied = false } }
    };
}

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    auto &state = state_for(_trusted_memory);
    const size_t wanted = required_block_size(size);

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
    const size_t remainder = block.size - wanted;
    const size_t block_offset = block.offset;
    block.occupied = true;
    block.size = wanted;

    if (remainder > block_metadata_size)
    {
        state.blocks.insert(
            state.blocks.begin() + static_cast<ptrdiff_t>(selected + 1),
            sorted_block{ .offset = block_offset + wanted, .size = remainder, .occupied = false });
    }
    else
    {
        block.size += remainder;
    }

    return reinterpret_cast<unsigned char *>(_trusted_memory) + block_offset + sorted_block_metadata_size;
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other):
    allocator_sorted_list(state_for(other._trusted_memory).space_size, state_for(other._trusted_memory).parent, state_for(other._trusted_memory).mode)
{
    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    const auto &other_state = state_for(other._trusted_memory);
    auto &this_state = state_for(_trusted_memory);
    std::memcpy(_trusted_memory, other._trusted_memory, other_state.space_size);
    this_state.blocks = other_state.blocks;
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{
    if (this == &other)
    {
        return *this;
    }

    auto copy(other);
    *this = std::move(copy);
    return *this;
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_sorted_list *>(&other) != nullptr;
}

void allocator_sorted_list::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    auto *ptr = reinterpret_cast<unsigned char *>(at);

    if (ptr < base + sorted_block_metadata_size || ptr >= base + state.space_size)
    {
        throw std::logic_error("allocator_sorted_list deallocate out of range");
    }

    const size_t offset = static_cast<size_t>(ptr - base - sorted_block_metadata_size);
    auto it = std::find_if(
        state.blocks.begin(),
        state.blocks.end(),
        [offset](const sorted_block &block)
        {
            return block.offset == offset;
        });

    if (it == state.blocks.end() || !it->occupied)
    {
        throw std::logic_error("allocator_sorted_list deallocate invalid block");
    }

    it->occupied = false;
    merge_sorted_blocks(state);
}

inline void allocator_sorted_list::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    state_for(_trusted_memory).mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    return get_blocks_info_inner();
}


std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
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

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    const auto &state = state_for(_trusted_memory);
    for (const auto &block : state.blocks)
    {
        if (!block.occupied)
        {
            return sorted_free_iterator(reinterpret_cast<unsigned char *>(_trusted_memory) + block.offset);
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
    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    const auto &state = state_for(_trusted_memory);
    if (state.blocks.empty())
    {
        return end();
    }

    sorted_iterator it;
    it._trusted_memory = _trusted_memory;
    it._current_ptr = reinterpret_cast<unsigned char *>(_trusted_memory) + state.blocks.front().offset;
    it._free_ptr = nullptr;
    return it;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    sorted_iterator it;
    it._trusted_memory = _trusted_memory;
    it._free_ptr = nullptr;
    it._current_ptr = nullptr;
    return it;
}


bool allocator_sorted_list::sorted_free_iterator::operator==(
        const allocator_sorted_list::sorted_free_iterator & other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(
        const allocator_sorted_list::sorted_free_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator &allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr == nullptr)
    {
        return *this;
    }

    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    for (const auto &[trusted, state] : g_sorted_states)
    {
        auto *base = reinterpret_cast<unsigned char *>(trusted);
        auto *current = reinterpret_cast<unsigned char *>(_free_ptr);
        if (current < base || current >= base + state.space_size)
        {
            continue;
        }

        const size_t offset = static_cast<size_t>(current - base);
        bool take_next = false;
        for (const auto &block : state.blocks)
        {
            if (!block.occupied)
            {
                if (take_next)
                {
                    _free_ptr = base + block.offset;
                    return *this;
                }
                if (block.offset == offset)
                {
                    take_next = true;
                }
            }
        }

        _free_ptr = nullptr;
        return *this;
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
    if (_free_ptr == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    for (const auto &[trusted, state] : g_sorted_states)
    {
        auto *base = reinterpret_cast<unsigned char *>(trusted);
        auto *current = reinterpret_cast<unsigned char *>(_free_ptr);
        if (current < base || current >= base + state.space_size)
        {
            continue;
        }
        const size_t offset = static_cast<size_t>(current - base);
        for (const auto &block : state.blocks)
        {
            if (!block.occupied && block.offset == offset)
            {
                return block.size;
            }
        }
    }

    return 0;
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

bool allocator_sorted_list::sorted_iterator::operator==(const allocator_sorted_list::sorted_iterator & other) const noexcept
{
    return _current_ptr == other._current_ptr && _trusted_memory == other._trusted_memory;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const allocator_sorted_list::sorted_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr == nullptr || _trusted_memory == nullptr)
    {
        return *this;
    }

    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    const auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_current_ptr) - base);
    for (size_t i = 0; i < state.blocks.size(); ++i)
    {
        if (state.blocks[i].offset == offset)
        {
            if (i + 1 < state.blocks.size())
            {
                _current_ptr = base + state.blocks[i + 1].offset;
            }
            else
            {
                _current_ptr = nullptr;
            }
            return *this;
        }
    }

    _current_ptr = nullptr;
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
    if (_current_ptr == nullptr || _trusted_memory == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    const auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_current_ptr) - base);
    for (const auto &block : state.blocks)
    {
        if (block.offset == offset)
        {
            return block.size;
        }
    }
    return 0;
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
    _trusted_memory(trusted)
{
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    if (_current_ptr == nullptr || _trusted_memory == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> guard(g_sorted_states_mutex);
    const auto &state = state_for(_trusted_memory);
    auto *base = reinterpret_cast<unsigned char *>(_trusted_memory);
    const size_t offset = static_cast<size_t>(reinterpret_cast<unsigned char *>(_current_ptr) - base);
    for (const auto &block : state.blocks)
    {
        if (block.offset == offset)
        {
            return block.occupied;
        }
    }

    return false;
}
