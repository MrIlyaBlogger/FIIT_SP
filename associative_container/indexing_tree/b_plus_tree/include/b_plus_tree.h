#ifndef SYS_PROG_B_PLUS_TREE_H
#define SYS_PROG_B_PLUS_TREE_H

#include <b_tree.h>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class BP_tree final : private compare
{
public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

private:
    using impl_type = B_tree<tkey, tvalue, compare, t>;
    impl_type _tree;

public:
    explicit BP_tree(const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>()) : compare(cmp), _tree(cmp, alloc) {}
    explicit BP_tree(pp_allocator<value_type> alloc, const compare &cmp = compare()) : compare(cmp), _tree(cmp, alloc) {}

    template<input_iterator_for_pair<tkey, tvalue> iterator>
    explicit BP_tree(iterator begin, iterator end, const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>()) : compare(cmp), _tree(begin, end, cmp, alloc) {}

    BP_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>()) : compare(cmp), _tree(data, cmp, alloc) {}

    BP_tree(const BP_tree &) = default;
    BP_tree(BP_tree &&) noexcept = default;
    BP_tree &operator=(const BP_tree &) = default;
    BP_tree &operator=(BP_tree &&) noexcept = default;
    ~BP_tree() noexcept = default;

    class bptree_iterator
    {
        typename impl_type::btree_iterator _it;
        friend class bptree_const_iterator;
    public:
        using value_type = tree_data_type_const;
        using reference = tree_data_type &;
        using pointer = tree_data_type *;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = bptree_iterator;
        explicit bptree_iterator(typename impl_type::btree_iterator it = {}) : _it(std::move(it)) {}
        reference operator*() const noexcept { return *_it; }
        pointer operator->() const noexcept { return _it.operator->(); }
        self &operator++() { ++_it; return *this; }
        self operator++(int) { auto c = *this; ++(*this); return c; }
        bool operator==(const self &other) const noexcept { return _it == other._it; }
        bool operator!=(const self &other) const noexcept { return !(*this == other); }
        size_t current_node_keys_count() const noexcept { return _it.current_node_keys_count(); }
        size_t index() const noexcept { return _it.index(); }
    };

    class bptree_const_iterator
    {
        typename impl_type::btree_const_iterator _it;
    public:
        using value_type = tree_data_type_const;
        using reference = const tree_data_type &;
        using pointer = const tree_data_type *;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = bptree_const_iterator;
        explicit bptree_const_iterator(typename impl_type::btree_const_iterator it = {}) : _it(std::move(it)) {}
        bptree_const_iterator(const bptree_iterator &it) noexcept : _it(it._it) {}
        reference operator*() const noexcept { return *_it; }
        pointer operator->() const noexcept { return _it.operator->(); }
        self &operator++() { ++_it; return *this; }
        self operator++(int) { auto c = *this; ++(*this); return c; }
        bool operator==(const self &other) const noexcept { return _it == other._it; }
        bool operator!=(const self &other) const noexcept { return !(*this == other); }
        size_t current_node_keys_count() const noexcept { return _it.current_node_keys_count(); }
        size_t index() const noexcept { return _it.index(); }
        friend class bptree_iterator;
    };

    tvalue &at(const tkey &key) { return _tree.at(key); }
    const tvalue &at(const tkey &key) const { return _tree.at(key); }
    tvalue &operator[](const tkey &key) { return _tree[key]; }
    tvalue &operator[](tkey &&key) { return _tree[std::move(key)]; }

    bptree_iterator begin() { return bptree_iterator(_tree.begin()); }
    bptree_iterator end() { return bptree_iterator(_tree.end()); }
    bptree_const_iterator begin() const { return bptree_const_iterator(_tree.begin()); }
    bptree_const_iterator end() const { return bptree_const_iterator(_tree.end()); }
    bptree_const_iterator cbegin() const { return bptree_const_iterator(_tree.cbegin()); }
    bptree_const_iterator cend() const { return bptree_const_iterator(_tree.cend()); }

    size_t size() const noexcept { return _tree.size(); }
    bool empty() const noexcept { return _tree.empty(); }
    bptree_iterator find(const tkey &key) { return bptree_iterator(_tree.find(key)); }
    bptree_const_iterator find(const tkey &key) const { return bptree_const_iterator(_tree.find(key)); }
    bptree_iterator lower_bound(const tkey &key) { return bptree_iterator(_tree.lower_bound(key)); }
    bptree_const_iterator lower_bound(const tkey &key) const { return bptree_const_iterator(_tree.lower_bound(key)); }
    bptree_iterator upper_bound(const tkey &key) { return bptree_iterator(_tree.upper_bound(key)); }
    bptree_const_iterator upper_bound(const tkey &key) const { return bptree_const_iterator(_tree.upper_bound(key)); }
    bool contains(const tkey &key) const { return _tree.contains(key); }

    void clear() noexcept { _tree.clear(); }
    std::pair<bptree_iterator, bool> insert(const tree_data_type &data) { auto [it, ok] = _tree.insert(data); return {bptree_iterator(it), ok}; }
    std::pair<bptree_iterator, bool> insert(tree_data_type &&data) { auto [it, ok] = _tree.insert(std::move(data)); return {bptree_iterator(it), ok}; }
    template <typename ...Args>
    std::pair<bptree_iterator, bool> emplace(Args&&... args) { auto [it, ok] = _tree.emplace(std::forward<Args>(args)...); return {bptree_iterator(it), ok}; }
    bptree_iterator erase(bptree_iterator pos) { return bptree_iterator(_tree.erase(pos._it)); }
    bptree_iterator erase(bptree_const_iterator pos) { return bptree_iterator(_tree.erase(static_cast<typename impl_type::btree_const_iterator>(pos._it))); }
    bptree_iterator erase(bptree_const_iterator begin, bptree_const_iterator end) { return bptree_iterator(_tree.erase(begin._it, end._it)); }
    bptree_iterator erase(const tkey &key) { return bptree_iterator(_tree.erase(key)); }

    pp_allocator<value_type> get_allocator() const noexcept { return _tree.get_allocator(); }
};

#endif
