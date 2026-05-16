#include "../include/allocator_red_black_tree.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>

namespace
{
    enum class color : unsigned char
    {
        red,
        black
    };

    constexpr size_t align_up(size_t value, size_t alignment) noexcept
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    struct rb_block
    {
        size_t size;
        rb_block *next;
        rb_block *previous;
        rb_block *parent;
        rb_block *left;
        rb_block *right;
        color node_color;
        bool occupied;
    };

    struct allocator_state
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t user_space_size;
        size_t allocated_size;
        rb_block *first;
        rb_block *root;
        std::mutex mutex;
    };

    constexpr size_t state_offset = align_up(sizeof(allocator_state), alignof(std::max_align_t));
    constexpr size_t block_metadata_size = sizeof(rb_block);

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

    bool less_block(const rb_block *lhs, const rb_block *rhs) noexcept
    {
        if (lhs->size != rhs->size)
        {
            return lhs->size < rhs->size;
        }
        return lhs < rhs;
    }

    void rotate_left(allocator_state *state, rb_block *node) noexcept
    {
        auto *right = node->right;
        node->right = right->left;
        if (right->left != nullptr)
        {
            right->left->parent = node;
        }

        right->parent = node->parent;
        if (node->parent == nullptr)
        {
            state->root = right;
        }
        else if (node == node->parent->left)
        {
            node->parent->left = right;
        }
        else
        {
            node->parent->right = right;
        }

        right->left = node;
        node->parent = right;
    }

    void rotate_right(allocator_state *state, rb_block *node) noexcept
    {
        auto *left = node->left;
        node->left = left->right;
        if (left->right != nullptr)
        {
            left->right->parent = node;
        }

        left->parent = node->parent;
        if (node->parent == nullptr)
        {
            state->root = left;
        }
        else if (node == node->parent->right)
        {
            node->parent->right = left;
        }
        else
        {
            node->parent->left = left;
        }

        left->right = node;
        node->parent = left;
    }

    void insert_free_block(allocator_state *state, rb_block *node) noexcept
    {
        node->left = nullptr;
        node->right = nullptr;
        node->parent = nullptr;
        node->node_color = color::red;

        rb_block *parent = nullptr;
        auto *current = state->root;
        while (current != nullptr)
        {
            parent = current;
            current = less_block(node, current) ? current->left : current->right;
        }

        node->parent = parent;
        if (parent == nullptr)
        {
            state->root = node;
        }
        else if (less_block(node, parent))
        {
            parent->left = node;
        }
        else
        {
            parent->right = node;
        }

        while (node->parent != nullptr && node->parent->node_color == color::red)
        {
            auto *grandparent = node->parent->parent;
            if (node->parent == grandparent->left)
            {
                auto *uncle = grandparent->right;
                if (uncle != nullptr && uncle->node_color == color::red)
                {
                    node->parent->node_color = color::black;
                    uncle->node_color = color::black;
                    grandparent->node_color = color::red;
                    node = grandparent;
                }
                else
                {
                    if (node == node->parent->right)
                    {
                        node = node->parent;
                        rotate_left(state, node);
                    }
                    node->parent->node_color = color::black;
                    grandparent->node_color = color::red;
                    rotate_right(state, grandparent);
                }
            }
            else
            {
                auto *uncle = grandparent->left;
                if (uncle != nullptr && uncle->node_color == color::red)
                {
                    node->parent->node_color = color::black;
                    uncle->node_color = color::black;
                    grandparent->node_color = color::red;
                    node = grandparent;
                }
                else
                {
                    if (node == node->parent->left)
                    {
                        node = node->parent;
                        rotate_right(state, node);
                    }
                    node->parent->node_color = color::black;
                    grandparent->node_color = color::red;
                    rotate_left(state, grandparent);
                }
            }
        }

        state->root->node_color = color::black;
    }

    void rebuild_free_tree(allocator_state *state) noexcept
    {
        state->root = nullptr;
        for (auto *block = state->first; block != nullptr; block = block->next)
        {
            block->parent = nullptr;
            block->left = nullptr;
            block->right = nullptr;
            block->node_color = color::black;
            if (!block->occupied)
            {
                insert_free_block(state, block);
            }
        }
    }

    size_t required_block_size(size_t payload) noexcept
    {
        return std::max(payload, size_t{1}) + block_metadata_size;
    }

    void split_if_possible(rb_block *block, size_t wanted) noexcept
    {
        const size_t remainder = block->size - wanted;
        if (remainder <= block_metadata_size)
        {
            return;
        }

        auto *created = reinterpret_cast<rb_block *>(reinterpret_cast<unsigned char *>(block) + wanted);
        created->size = remainder;
        created->next = block->next;
        created->previous = block;
        created->occupied = false;
        if (created->next != nullptr)
        {
            created->next->previous = created;
        }

        block->size = wanted;
        block->next = created;
    }

    void merge_with_next(rb_block *block) noexcept
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

    rb_block *find_block_by_payload(allocator_state *state, void *payload)
    {
        const auto *begin = reinterpret_cast<const unsigned char *>(state->first);
        const auto *end = begin + state->user_space_size;
        auto *ptr = static_cast<unsigned char *>(payload);

        if (ptr < begin + block_metadata_size || ptr >= end)
        {
            throw std::logic_error("allocator_red_black_tree deallocate out of range");
        }

        for (auto *block = state->first; block != nullptr; block = block->next)
        {
            if (reinterpret_cast<unsigned char *>(block) + block_metadata_size == ptr)
            {
                return block;
            }
        }

        throw std::logic_error("allocator_red_black_tree deallocate invalid block");
    }

    rb_block *select_block(allocator_state *state, size_t wanted) noexcept
    {
        if (state->root == nullptr)
        {
            return nullptr;
        }

        if (state->mode == allocator_with_fit_mode::fit_mode::the_worst_fit)
        {
            auto *node = state->root;
            while (node->right != nullptr)
            {
                node = node->right;
            }
            return node->size >= wanted ? node : nullptr;
        }

        // first_fit and the_best_fit both map naturally to lower_bound(size>=wanted)
        // in the RB tree ordered by (size, address).
        rb_block *candidate = nullptr;
        auto *current = state->root;
        while (current != nullptr)
        {
            if (current->size >= wanted)
            {
                candidate = current;
                current = current->left;
            }
            else
            {
                current = current->right;
            }
        }

        return candidate;
    }

    void rebuild_links(allocator_state *state) noexcept
    {
        auto *cursor = reinterpret_cast<rb_block *>(pool_begin(state));
        auto *end = pool_begin(state) + state->user_space_size;
        rb_block *previous = nullptr;
        state->first = cursor;

        while (reinterpret_cast<unsigned char *>(cursor) < end)
        {
            cursor->previous = previous;
            auto *next_address = reinterpret_cast<unsigned char *>(cursor) + cursor->size;
            cursor->next = next_address < end ? reinterpret_cast<rb_block *>(next_address) : nullptr;
            previous = cursor;
            cursor = cursor->next;
        }
        rebuild_free_tree(state);
    }
}

allocator_red_black_tree::allocator_red_black_tree(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < block_metadata_size + 1)
    {
        throw std::logic_error("allocator_red_black_tree requires more memory");
    }

    auto *resource = parent_allocator == nullptr ? std::pmr::get_default_resource() : parent_allocator;
    const size_t managed_space_size = space_size + std::max(space_size / 2, block_metadata_size * size_t{64});
    const size_t allocated_size = state_offset + managed_space_size;
    _trusted_memory = resource->allocate(allocated_size, alignof(std::max_align_t));

    try
    {
        auto *state = new (_trusted_memory) allocator_state{
            .parent = resource,
            .mode = allocate_fit_mode,
            .user_space_size = managed_space_size,
            .allocated_size = allocated_size,
            .first = reinterpret_cast<rb_block *>(pool_begin(_trusted_memory)),
            .root = nullptr,
            .mutex = {}
        };

        state->first->size = managed_space_size;
        state->first->next = nullptr;
        state->first->previous = nullptr;
        state->first->occupied = false;
        rebuild_free_tree(state);
    }
    catch (...)
    {
        resource->deallocate(_trusted_memory, allocated_size, alignof(std::max_align_t));
        _trusted_memory = nullptr;
        throw;
    }
}

allocator_red_black_tree::~allocator_red_black_tree()
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

allocator_red_black_tree::allocator_red_black_tree(allocator_red_black_tree &&other) noexcept:
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_red_black_tree &allocator_red_black_tree::operator=(allocator_red_black_tree &&other) noexcept
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

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &other)
{
    auto *other_state = state_of(other._trusted_memory);
    allocator_red_black_tree copy(other_state->user_space_size, other_state->parent, other_state->mode);

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

allocator_red_black_tree &allocator_red_black_tree::operator=(const allocator_red_black_tree &other)
{
    if (this == &other)
    {
        return *this;
    }

    allocator_red_black_tree copy(other);
    *this = std::move(copy);
    return *this;
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_red_black_tree *>(&other) != nullptr;
}

[[nodiscard]] void *allocator_red_black_tree::do_allocate_sm(size_t size)
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    const size_t wanted = required_block_size(size);
    auto *selected = select_block(state, wanted);

    if (selected == nullptr)
    {
        throw std::bad_alloc();
    }

    selected->occupied = true;
    split_if_possible(selected, wanted);
    rebuild_free_tree(state);
    return reinterpret_cast<unsigned char *>(selected) + block_metadata_size;
}

void allocator_red_black_tree::do_deallocate_sm(void *at)
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
        throw std::logic_error("allocator_red_black_tree deallocate invalid block");
    }

    block->occupied = false;
    merge_with_next(block);
    if (block->previous != nullptr && !block->previous->occupied)
    {
        block = block->previous;
        merge_with_next(block);
    }
    rebuild_free_tree(state);
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const
{
    const auto *state = state_of(_trusted_memory);
    std::vector<allocator_test_utils::block_info> result;
    for (auto *block = state->first; block != nullptr; block = block->next)
    {
        result.push_back({ .block_size = block->size, .is_block_occupied = block->occupied });
    }
    return result;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept
{
    auto *state = state_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(state->mutex);
    return rb_iterator(state->first);
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept
{
    return rb_iterator();
}

bool allocator_red_black_tree::rb_iterator::operator==(const rb_iterator &other) const noexcept
{
    return _block_ptr == other._block_ptr;
}

bool allocator_red_black_tree::rb_iterator::operator!=(const rb_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_red_black_tree::rb_iterator &allocator_red_black_tree::rb_iterator::operator++() & noexcept
{
    auto *block = static_cast<rb_block *>(_block_ptr);
    _block_ptr = block == nullptr ? nullptr : block->next;
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
    auto *block = static_cast<rb_block *>(_block_ptr);
    return block == nullptr ? 0 : block->size;
}

void *allocator_red_black_tree::rb_iterator::operator*() const noexcept
{
    return _block_ptr;
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept
{
    auto *block = static_cast<rb_block *>(_block_ptr);
    return block != nullptr && block->occupied;
}

allocator_red_black_tree::rb_iterator::rb_iterator():
    _block_ptr(nullptr),
    _trusted(nullptr)
{
}

allocator_red_black_tree::rb_iterator::rb_iterator(void *trusted):
    _block_ptr(trusted),
    _trusted(nullptr)
{
}
