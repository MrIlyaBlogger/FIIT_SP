#ifndef SYS_PROG_B_TREE_IMPL_H
#define SYS_PROG_B_TREE_IMPL_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <associative_container.h>
#include <pp_allocator.h>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class B_tree final : private compare
{
public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

    class btree_exception : public std::runtime_error
    {
    public:
        explicit btree_exception(const char *message) : std::runtime_error(message) {}
    };

    class key_not_found final : public btree_exception
    {
    public:
        key_not_found() : btree_exception("B_tree key not found") {}
    };

private:
    static constexpr size_t maximum_keys_in_node = 2 * t - 1;

    struct node
    {
        std::pmr::vector<tree_data_type> keys;
        std::pmr::vector<node *> children;

        explicit node(std::pmr::memory_resource *resource = std::pmr::get_default_resource()) :
            keys(resource),
            children(resource)
        {
        }

        bool leaf() const noexcept
        {
            return children.empty();
        }
    };

    struct place
    {
        node *where;
        size_t index;
        size_t depth;
    };

    struct const_place
    {
        const node *where;
        size_t index;
        size_t depth;
    };

    struct split_result
    {
        tree_data_type promoted;
        node *right;
    };

    pp_allocator<value_type> _allocator;
    node *_root = nullptr;
    size_t _size = 0;

    using node_allocator = pp_allocator<node>;

    bool less_key(const tkey &lhs, const tkey &rhs) const
    {
        return compare::operator()(lhs, rhs);
    }

    bool equivalent_key(const tkey &lhs, const tkey &rhs) const
    {
        return !less_key(lhs, rhs) && !less_key(rhs, lhs);
    }

    size_t lower_index(const std::pmr::vector<tree_data_type> &keys, const tkey &key) const
    {
        const auto it = std::lower_bound(
            keys.begin(),
            keys.end(),
            key,
            [this](const tree_data_type &data, const tkey &value) {
                return less_key(data.first, value);
            });
        return static_cast<size_t>(std::distance(keys.begin(), it));
    }

    node_allocator nodes_allocator() const noexcept
    {
        return node_allocator(_allocator.resource());
    }

    node *new_node()
    {
        auto allocator = nodes_allocator();
        return allocator.template new_object<node>(_allocator.resource());
    }

    void delete_node(node *ptr) noexcept
    {
        if (ptr == nullptr)
        {
            return;
        }

        auto allocator = nodes_allocator();
        allocator.delete_object(ptr);
    }

    void delete_subtree(node *ptr) noexcept
    {
        if (ptr == nullptr)
        {
            return;
        }

        for (auto *child : ptr->children)
        {
            delete_subtree(child);
        }
        delete_node(ptr);
    }

    node *copy_subtree(const node *ptr)
    {
        if (ptr == nullptr)
        {
            return nullptr;
        }

        auto *result = new_node();
        try
        {
            result->keys = ptr->keys;
            result->children.reserve(ptr->children.size());
            for (const auto *child : ptr->children)
            {
                result->children.push_back(copy_subtree(child));
            }
        }
        catch (...)
        {
            delete_subtree(result);
            throw;
        }
        return result;
    }

    split_result split_node(node *ptr)
    {
        const size_t median = ptr->keys.size() / 2;
        auto *right = new_node();

        try
        {
            right->keys.assign(
                std::make_move_iterator(ptr->keys.begin() + static_cast<ptrdiff_t>(median + 1)),
                std::make_move_iterator(ptr->keys.end()));

            if (!ptr->leaf())
            {
                right->children.assign(
                    ptr->children.begin() + static_cast<ptrdiff_t>(median + 1),
                    ptr->children.end());
                ptr->children.erase(
                    ptr->children.begin() + static_cast<ptrdiff_t>(median + 1),
                    ptr->children.end());
            }

            tree_data_type promoted = std::move(ptr->keys[median]);
            ptr->keys.erase(ptr->keys.begin() + static_cast<ptrdiff_t>(median), ptr->keys.end());
            return {std::move(promoted), right};
        }
        catch (...)
        {
            delete_node(right);
            throw;
        }
    }

    split_result insert_inner(node *ptr, tree_data_type &&data)
    {
        const size_t index = lower_index(ptr->keys, data.first);

        if (ptr->leaf())
        {
            ptr->keys.insert(ptr->keys.begin() + static_cast<ptrdiff_t>(index), std::move(data));
        }
        else
        {
            split_result child_split = insert_inner(ptr->children[index], std::move(data));
            if (child_split.right != nullptr)
            {
                ptr->keys.insert(ptr->keys.begin() + static_cast<ptrdiff_t>(index), std::move(child_split.promoted));
                ptr->children.insert(ptr->children.begin() + static_cast<ptrdiff_t>(index + 1), child_split.right);
            }
        }

        if (ptr->keys.size() <= maximum_keys_in_node)
        {
            return {tree_data_type{}, nullptr};
        }
        return split_node(ptr);
    }

    void collect(node *ptr, size_t depth, std::vector<place> &out)
    {
        if (ptr == nullptr)
        {
            return;
        }

        for (size_t i = 0; i < ptr->keys.size(); ++i)
        {
            if (!ptr->leaf())
            {
                collect(ptr->children[i], depth + 1, out);
            }
            out.push_back({ptr, i, depth});
        }

        if (!ptr->leaf())
        {
            collect(ptr->children.back(), depth + 1, out);
        }
    }

    void collect(const node *ptr, size_t depth, std::vector<const_place> &out) const
    {
        if (ptr == nullptr)
        {
            return;
        }

        for (size_t i = 0; i < ptr->keys.size(); ++i)
        {
            if (!ptr->leaf())
            {
                collect(ptr->children[i], depth + 1, out);
            }
            out.push_back({ptr, i, depth});
        }

        if (!ptr->leaf())
        {
            collect(ptr->children.back(), depth + 1, out);
        }
    }

    std::vector<place> order()
    {
        std::vector<place> result;
        result.reserve(_size);
        collect(_root, 0, result);
        return result;
    }

    std::vector<const_place> order() const
    {
        std::vector<const_place> result;
        result.reserve(_size);
        collect(_root, 0, result);
        return result;
    }

    tree_data_type take_greatest(node *ptr)
    {
        if (ptr->leaf())
        {
            tree_data_type result = std::move(ptr->keys.back());
            ptr->keys.pop_back();
            return result;
        }
        return take_greatest(ptr->children.back());
    }


    tree_data_type *find_data_ptr(const tkey &key)
    {
        node *current = _root;
        while (current != nullptr)
        {
            const size_t index = lower_index(current->keys, key);
            if (index < current->keys.size() && equivalent_key(current->keys[index].first, key))
            {
                return &current->keys[index];
            }
            if (current->leaf())
            {
                break;
            }
            current = current->children[index];
        }
        return nullptr;
    }

    const tree_data_type *find_data_ptr(const tkey &key) const
    {
        const node *current = _root;
        while (current != nullptr)
        {
            const size_t index = lower_index(current->keys, key);
            if (index < current->keys.size() && equivalent_key(current->keys[index].first, key))
            {
                return &current->keys[index];
            }
            if (current->leaf())
            {
                break;
            }
            current = current->children[index];
        }
        return nullptr;
    }

    const node *leftmost_node() const
    {
        const node *current = _root;
        while (current != nullptr && !current->leaf()) current = current->children.front();
        return current;
    }

    const node *rightmost_node() const
    {
        const node *current = _root;
        while (current != nullptr && !current->leaf()) current = current->children.back();
        return current;
    }

    template<typename Iter>
    void next_key(Iter &it) const
    {
        if (it._is_end)
        {
            return;
        }

        auto items = order();
        for (size_t i = 0; i < items.size(); ++i)
        {
            const tkey &current_key = items[i].where->keys[items[i].index].first;
            if (equivalent_key(current_key, it._key.value()))
            {
                if (i + 1 >= items.size())
                {
                    it._is_end = true;
                    it._key.reset();
                }
                else
                {
                    it._key = items[i + 1].where->keys[items[i + 1].index].first;
                }
                return;
            }
        }

        throw std::out_of_range("B_tree iterator is invalidated");
    }

    template<typename Iter>
    void prev_key(Iter &it) const
    {
        auto items = order();

        if (it._is_end)
        {
            if (items.empty())
            {
                return;
            }
            it._is_end = false;
            it._key = items.back().where->keys[items.back().index].first;
            return;
        }

        for (size_t i = 0; i < items.size(); ++i)
        {
            const tkey &current_key = items[i].where->keys[items[i].index].first;
            if (equivalent_key(current_key, it._key.value()))
            {
                if (i == 0)
                {
                    it._is_end = true;
                    it._key.reset();
                }
                else
                {
                    it._key = items[i - 1].where->keys[items[i - 1].index].first;
                }
                return;
            }
        }

        throw std::out_of_range("B_tree iterator is invalidated");
    }

    size_t depth_of_key(const tkey &key) const
    {
        const node *current = _root; size_t depth = 0;
        while (current != nullptr)
        {
            size_t idx = lower_index(current->keys, key);
            if (idx < current->keys.size() && equivalent_key(current->keys[idx].first, key)) return depth;
            if (current->leaf()) break;
            current = current->children[idx]; ++depth;
        }
        throw std::out_of_range("B_tree iterator is invalidated");
    }
    size_t index_of_key(const tkey &key) const
    {
        const node *current = _root;
        while (current != nullptr)
        {
            size_t idx = lower_index(current->keys, key);
            if (idx < current->keys.size() && equivalent_key(current->keys[idx].first, key)) return idx;
            if (current->leaf()) break;
            current = current->children[idx];
        }
        throw std::out_of_range("B_tree iterator is invalidated");
    }
    size_t node_key_count(const tkey &key) const
    {
        const node *current = _root;
        while (current != nullptr)
        {
            size_t idx = lower_index(current->keys, key);
            if (idx < current->keys.size() && equivalent_key(current->keys[idx].first, key)) return current->keys.size();
            if (current->leaf()) break;
            current = current->children[idx];
        }
        throw std::out_of_range("B_tree iterator is invalidated");
    }
    bool node_is_leaf(const tkey &key) const
    {
        const node *current = _root;
        while (current != nullptr)
        {
            size_t idx = lower_index(current->keys, key);
            if (idx < current->keys.size() && equivalent_key(current->keys[idx].first, key)) return current->leaf();
            if (current->leaf()) break;
            current = current->children[idx];
        }
        throw std::out_of_range("B_tree iterator is invalidated");
    }

    bool erase_inner(node *ptr, const tkey &key)
    {
        const size_t index = lower_index(ptr->keys, key);
        if (index < ptr->keys.size() && equivalent_key(ptr->keys[index].first, key))
        {
            if (ptr->leaf())
            {
                ptr->keys.erase(ptr->keys.begin() + static_cast<ptrdiff_t>(index));
            }
            else
            {
                ptr->keys[index] = take_greatest(ptr->children[index]);
            }
            return true;
        }

        return !ptr->leaf() && erase_inner(ptr->children[index], key);
    }

public:
    explicit B_tree(const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>()) :
        compare(cmp),
        _allocator(alloc)
    {
        static_assert(t > 1, "B_tree parameter t must be greater than 1");
    }

    explicit B_tree(pp_allocator<value_type> alloc, const compare &comp = compare()) :
        B_tree(comp, alloc)
    {
    }

    template<input_iterator_for_pair<tkey, tvalue> iterator>
    explicit B_tree(iterator begin, iterator end, const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>()) :
        B_tree(cmp, alloc)
    {
        for (auto it = begin; it != end; ++it)
        {
            insert(*it);
        }
    }

    B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>()) :
        B_tree(data.begin(), data.end(), cmp, alloc)
    {
    }

    B_tree(const B_tree &other) :
        compare(static_cast<const compare &>(other)),
        _allocator(other._allocator.select_on_container_copy_construction()),
        _root(copy_subtree(other._root)),
        _size(other._size)
    {
    }

    B_tree(B_tree &&other) noexcept :
        compare(std::move(static_cast<compare &>(other))),
        _allocator(std::move(other._allocator)),
        _root(other._root),
        _size(other._size)
    {
        other._root = nullptr;
        other._size = 0;
    }

    B_tree &operator=(const B_tree &other)
    {
        if (this == &other)
        {
            return *this;
        }

        B_tree copy(other);
        *this = std::move(copy);
        return *this;
    }

    B_tree &operator=(B_tree &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        clear();
        static_cast<compare &>(*this) = std::move(static_cast<compare &>(other));
        _allocator = std::move(other._allocator);
        _root = other._root;
        _size = other._size;
        other._root = nullptr;
        other._size = 0;
        return *this;
    }

    ~B_tree() noexcept
    {
        clear();
    }

    class btree_iterator;
    class btree_reverse_iterator;
    class btree_const_iterator;
    class btree_const_reverse_iterator;

    class btree_iterator final
    {
        B_tree *_owner = nullptr;
        std::optional<tkey> _key;
        bool _is_end = true;

        tree_data_type *current_data() const
        {
            if (_owner == nullptr || _is_end || !_key.has_value())
            {
                throw std::out_of_range("B_tree iterator is not dereferenceable");
            }
            tree_data_type *data = _owner->find_data_ptr(_key.value());
            if (data == nullptr)
            {
                throw std::out_of_range("B_tree iterator is invalidated");
            }
            return data;
        }

    public:
        using value_type = tree_data_type_const;
        using reference = tree_data_type &;
        using pointer = tree_data_type *;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_iterator;

        friend class B_tree;
        friend class btree_const_iterator;

        explicit btree_iterator(B_tree *owner = nullptr, bool is_end = true) : _owner(owner), _is_end(is_end) {}
        explicit btree_iterator(B_tree *owner, const tkey &key) : _owner(owner), _key(key), _is_end(false) {}

        reference operator*() const { return *current_data(); }
        pointer operator->() const { return current_data(); }

        self &operator++() { _owner->next_key(*this); return *this; }
        self operator++(int) { auto copy = *this; ++(*this); return copy; }
        self &operator--() { _owner->prev_key(*this); return *this; }
        self operator--(int) { auto copy = *this; --(*this); return copy; }

        bool operator==(const self &other) const noexcept
        {
            if (_owner != other._owner || _is_end != other._is_end) return false;
            if (_is_end) return true;
            return _key == other._key;
        }
        bool operator!=(const self &other) const noexcept { return !(*this == other); }

        size_t depth() const { return _owner->depth_of_key(_key.value()); }
        size_t current_node_keys_count() const { return _owner->node_key_count(_key.value()); }
        bool is_terminate_node() const { return _owner->node_is_leaf(_key.value()); }
        size_t index() const { return _owner->index_of_key(_key.value()); }
    };

    class btree_const_iterator final
    {
        const B_tree *_owner = nullptr;
        std::optional<tkey> _key;
        bool _is_end = true;

        const tree_data_type *current_data() const
        {
            if (_owner == nullptr || _is_end || !_key.has_value())
            {
                throw std::out_of_range("B_tree const iterator is not dereferenceable");
            }
            const tree_data_type *data = _owner->find_data_ptr(_key.value());
            if (data == nullptr)
            {
                throw std::out_of_range("B_tree const iterator is invalidated");
            }
            return data;
        }

    public:
        using value_type = tree_data_type_const;
        using reference = const tree_data_type &;
        using pointer = const tree_data_type *;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_iterator;

        friend class B_tree;

        explicit btree_const_iterator(const B_tree *owner = nullptr, bool is_end = true) : _owner(owner), _is_end(is_end) {}
        explicit btree_const_iterator(const B_tree *owner, const tkey &key) : _owner(owner), _key(key), _is_end(false) {}
        btree_const_iterator(const btree_iterator &it) : _owner(it._owner), _key(it._key), _is_end(it._is_end) {}

        reference operator*() const { return *current_data(); }
        pointer operator->() const { return current_data(); }
        self &operator++() { _owner->next_key(*this); return *this; }
        self operator++(int) { auto copy = *this; ++(*this); return copy; }
        self &operator--() { _owner->prev_key(*this); return *this; }
        self operator--(int) { auto copy = *this; --(*this); return copy; }
        bool operator==(const self &other) const noexcept
        {
            if (_owner != other._owner || _is_end != other._is_end) return false;
            if (_is_end) return true;
            return _key == other._key;
        }
        bool operator!=(const self &other) const noexcept { return !(*this == other); }
        size_t depth() const { return _owner->depth_of_key(_key.value()); }
        size_t current_node_keys_count() const { return _owner->node_key_count(_key.value()); }
        bool is_terminate_node() const { return _owner->node_is_leaf(_key.value()); }
        size_t index() const { return _owner->index_of_key(_key.value()); }
    };

    class btree_reverse_iterator final
    {
        btree_iterator _base;
    public:
        using value_type = tree_data_type_const; using reference = tree_data_type &; using pointer = tree_data_type *; using iterator_category = std::bidirectional_iterator_tag; using difference_type = ptrdiff_t; using self = btree_reverse_iterator;
        explicit btree_reverse_iterator(B_tree *owner = nullptr, bool is_end = true) : _base(owner, is_end) {}
        explicit btree_reverse_iterator(B_tree *owner, const tkey &key) : _base(owner, key) {}
        btree_reverse_iterator(const btree_iterator &it) noexcept : _base(it) {}
        operator btree_iterator() const noexcept { return _base; }
        reference operator*() const { return *_base; }
        pointer operator->() const { return _base.operator->(); }
        self &operator++() { --_base; return *this; }
        self operator++(int) { auto copy=*this; ++(*this); return copy; }
        self &operator--() { ++_base; return *this; }
        self operator--(int) { auto copy=*this; --(*this); return copy; }
        bool operator==(const self &other) const noexcept { return _base==other._base; }
        bool operator!=(const self &other) const noexcept { return !(*this==other); }
        size_t depth() const { return _base.depth(); } size_t current_node_keys_count() const { return _base.current_node_keys_count(); } bool is_terminate_node() const { return _base.is_terminate_node(); } size_t index() const { return _base.index(); }
    };

    class btree_const_reverse_iterator final
    {
        btree_const_iterator _base;
    public:
        using value_type = tree_data_type_const; using reference = const tree_data_type &; using pointer = const tree_data_type *; using iterator_category = std::bidirectional_iterator_tag; using difference_type = ptrdiff_t; using self = btree_const_reverse_iterator;
        explicit btree_const_reverse_iterator(const B_tree *owner = nullptr, bool is_end = true) : _base(owner, is_end) {}
        explicit btree_const_reverse_iterator(const B_tree *owner, const tkey &key) : _base(owner, key) {}
        btree_const_reverse_iterator(const btree_reverse_iterator &it) : _base(static_cast<btree_iterator>(it)) {}
        operator btree_const_iterator() const noexcept { return _base; }
        reference operator*() const { return *_base; }
        pointer operator->() const { return _base.operator->(); }
        self &operator++() { --_base; return *this; }
        self operator++(int) { auto copy=*this; ++(*this); return copy; }
        self &operator--() { ++_base; return *this; }
        self operator--(int) { auto copy=*this; --(*this); return copy; }
        bool operator==(const self &other) const noexcept { return _base==other._base; }
        bool operator!=(const self &other) const noexcept { return !(*this==other); }
        size_t depth() const { return _base.depth(); } size_t current_node_keys_count() const { return _base.current_node_keys_count(); } bool is_terminate_node() const { return _base.is_terminate_node(); } size_t index() const { return _base.index(); }
    };

    tvalue &at(const tkey &key)
    {
        tree_data_type *data = find_data_ptr(key);
        if (data == nullptr)
        {
            throw key_not_found();
        }
        return data->second;
    }

    const tvalue &at(const tkey &key) const
    {
        const tree_data_type *data = find_data_ptr(key);
        if (data == nullptr)
        {
            throw key_not_found();
        }
        return data->second;
    }

    tvalue &operator[](const tkey &key)
    {
        auto [it, _] = emplace(key, tvalue{});
        return it->second;
    }

    tvalue &operator[](tkey &&key)
    {
        auto [it, inserted] = emplace(std::move(key), tvalue{});
        (void)inserted;
        return it->second;
    }

    btree_iterator begin() {
        const node *ln = leftmost_node();
        return (ln == nullptr || ln->keys.empty()) ? end() : btree_iterator(this, ln->keys.front().first);
    }
    btree_iterator end() { return btree_iterator(this, true); }
    btree_const_iterator begin() const { return cbegin(); }
    btree_const_iterator end() const { return cend(); }
    btree_const_iterator cbegin() const {
        const node *ln = leftmost_node();
        return (ln == nullptr || ln->keys.empty()) ? cend() : btree_const_iterator(this, ln->keys.front().first);
    }
    btree_const_iterator cend() const { return btree_const_iterator(this, true); }
    btree_reverse_iterator rbegin() {
        const node *rn = rightmost_node();
        return (rn == nullptr || rn->keys.empty()) ? rend() : btree_reverse_iterator(this, rn->keys.back().first);
    }
    btree_reverse_iterator rend() { return btree_reverse_iterator(this, true); }
    btree_const_reverse_iterator rbegin() const {
        const node *rn = rightmost_node();
        return (rn == nullptr || rn->keys.empty()) ? rend() : btree_const_reverse_iterator(this, rn->keys.back().first);
    }
    btree_const_reverse_iterator rend() const { return btree_const_reverse_iterator(this, true); }
    btree_const_reverse_iterator crbegin() const { return rbegin(); }
    btree_const_reverse_iterator crend() const { return rend(); }

    size_t size() const noexcept { return _size; }
    bool empty() const noexcept { return _size == 0; }

    btree_iterator find(const tkey &key)
    {
        tree_data_type *data = find_data_ptr(key);
        return data == nullptr ? end() : btree_iterator(this, data->first);
    }

    btree_const_iterator find(const tkey &key) const
    {
        const tree_data_type *data = find_data_ptr(key);
        return data == nullptr ? cend() : btree_const_iterator(this, data->first);
    }

    btree_iterator lower_bound(const tkey &key)
    {
        auto items = order();
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (!less_key(items[i].where->keys[items[i].index].first, key))
            {
                return btree_iterator(this, items[i].where->keys[items[i].index].first);
            }
        }
        return end();
    }

    btree_const_iterator lower_bound(const tkey &key) const
    {
        auto items = order();
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (!less_key(items[i].where->keys[items[i].index].first, key))
            {
                return btree_const_iterator(this, items[i].where->keys[items[i].index].first);
            }
        }
        return cend();
    }

    btree_iterator upper_bound(const tkey &key)
    {
        auto items = order();
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (less_key(key, items[i].where->keys[items[i].index].first))
            {
                return btree_iterator(this, items[i].where->keys[items[i].index].first);
            }
        }
        return end();
    }

    btree_const_iterator upper_bound(const tkey &key) const
    {
        auto items = order();
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (less_key(key, items[i].where->keys[items[i].index].first))
            {
                return btree_const_iterator(this, items[i].where->keys[items[i].index].first);
            }
        }
        return cend();
    }

    bool contains(const tkey &key) const
    {
        return find_data_ptr(key) != nullptr;
    }

    void clear() noexcept
    {
        delete_subtree(_root);
        _root = nullptr;
        _size = 0;
    }

    std::pair<btree_iterator, bool> insert(const tree_data_type &data)
    {
        return emplace(data.first, data.second);
    }

    std::pair<btree_iterator, bool> insert(tree_data_type &&data)
    {
        return emplace(std::move(data.first), std::move(data.second));
    }

    template <typename ...Args>
    std::pair<btree_iterator, bool> emplace(Args &&... args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        const tree_data_type *existing = find_data_ptr(data.first);
        if (existing != nullptr)
        {
            return {find(existing->first), false};
        }

        if (_root == nullptr)
        {
            _root = new_node();
        }

        const tkey inserted_key = data.first;
        split_result root_split = insert_inner(_root, std::move(data));
        if (root_split.right != nullptr)
        {
            auto *new_root = new_node();
            try
            {
                new_root->keys.push_back(std::move(root_split.promoted));
                new_root->children.push_back(_root);
                new_root->children.push_back(root_split.right);
                _root = new_root;
            }
            catch (...)
            {
                delete_node(new_root);
                throw;
            }
        }

        ++_size;
        return {find(inserted_key), true};
    }

    btree_iterator insert_or_assign(const tree_data_type &data)
    {
        auto it = find(data.first);
        if (it != end())
        {
            it->second = data.second;
            return it;
        }
        return insert(data).first;
    }

    btree_iterator insert_or_assign(tree_data_type &&data)
    {
        auto it = find(data.first);
        if (it != end())
        {
            it->second = std::move(data.second);
            return it;
        }
        return insert(std::move(data)).first;
    }

    template <typename ...Args>
    btree_iterator emplace_or_assign(Args &&... args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        return insert_or_assign(std::move(data));
    }

    btree_iterator erase(const tkey &key)
    {
        auto items = order();
        size_t position = items.size();
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (equivalent_key(items[i].where->keys[items[i].index].first, key))
            {
                position = i;
                break;
            }
        }

        if (position == items.size())
        {
            return end();
        }

        erase_inner(_root, key);
        --_size;

        if (_root != nullptr && _root->keys.empty() && !_root->leaf())
        {
            auto *old_root = _root;
            _root = _root->children.front();
            old_root->children.clear();
            delete_node(old_root);
        }
        if (_root != nullptr && _root->keys.empty() && _root->leaf())
        {
            delete_node(_root);
            _root = nullptr;
        }

        if (_size == 0 || position >= _size) return end();
        auto updated = order();
        return btree_iterator(this, updated[position].where->keys[updated[position].index].first);
    }

    btree_iterator erase(btree_iterator pos)
    {
        if (pos == end())
        {
            return end();
        }
        return erase(pos->first);
    }

    btree_iterator erase(btree_const_iterator pos)
    {
        if (pos == cend())
        {
            return end();
        }
        return erase(pos->first);
    }

    btree_iterator erase(btree_iterator beg, btree_iterator en)
    {
        while (beg != en)
        {
            beg = erase(beg);
        }
        return beg;
    }

    btree_iterator erase(btree_const_iterator beg, btree_const_iterator en)
    {
        while (beg != en)
        {
            auto key = beg->first;
            ++beg;
            erase(key);
        }
        return end();
    }
};

#endif
