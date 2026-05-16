#ifndef SYS_PROG_B_PLUS_TREE_H
#define SYS_PROG_B_PLUS_TREE_H

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <associative_container.h>
#include <pp_allocator.h>

template <typename tkey, typename tvalue,
          comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class BP_tree final : private compare {
public:
  using tree_data_type = std::pair<tkey, tvalue>;
  using tree_data_type_const = std::pair<const tkey, tvalue>;
  using value_type = tree_data_type_const;

  class bptree_exception : public std::runtime_error {
  public:
    explicit bptree_exception(const char *m) : std::runtime_error(m) {}
  };
  class key_not_found final : public bptree_exception {
  public:
    key_not_found() : bptree_exception("BP_tree key not found") {}
  };

private:
  static constexpr size_t max_keys = 2 * t - 1;
  struct node_base {
    bool is_leaf;

    explicit node_base(bool leaf) : is_leaf(leaf) {}

    virtual ~node_base() = default;
  };
  struct leaf_node final : node_base {
    std::vector<tree_data_type> data;
    leaf_node *next = nullptr;
    leaf_node *prev = nullptr;

    leaf_node() : node_base(true) {}
  };
  struct inner_node final : node_base {
    std::vector<tkey> keys;
    std::vector<std::unique_ptr<node_base>> children;

    inner_node() : node_base(false) {}
  };

  pp_allocator<value_type> _allocator;
  std::unique_ptr<node_base> _root;
  leaf_node *_first_leaf = nullptr;
  leaf_node *_last_leaf = nullptr;
  size_t _size = 0;

  bool less_key(const tkey &a, const tkey &b) const {
    return compare::operator()(a, b);
  }
  bool eq_key(const tkey &a, const tkey &b) const {
    return !less_key(a, b) && !less_key(b, a);
  }

  static size_t lower_idx(const std::vector<tree_data_type> &v, const tkey &k,
                          const BP_tree *self) {
    return static_cast<size_t>(
        std::lower_bound(v.begin(), v.end(), k,
                         [self](const tree_data_type &d, const tkey &x) {
                           return self->less_key(d.first, x);
                         }) -
        v.begin());
  }
  static size_t lower_idx_keys(const std::vector<tkey> &v, const tkey &k,
                               const BP_tree *self) {
    return static_cast<size_t>(
        std::lower_bound(v.begin(), v.end(), k,
                         [self](const tkey &d, const tkey &x) {
                           return self->less_key(d, x);
                         }) -
        v.begin());
  }

  leaf_node *leftmost_leaf() const { return _first_leaf; }

  leaf_node *find_leaf(const tkey &key) const {
    if (!_root)
      return nullptr;
    node_base *cur = _root.get();
    while (!cur->is_leaf) {
      auto *in = static_cast<inner_node *>(cur);
      size_t i = lower_idx_keys(in->keys, key, this);
      cur = in->children[i].get();
    }
    return static_cast<leaf_node *>(cur);
  }

  struct split_info {
    tkey sep{};
    std::unique_ptr<node_base> right{};
    bool has = false;
  };

  split_info insert_rec(node_base *cur, tree_data_type &&data) {
    if (cur->is_leaf) {
      auto *lf = static_cast<leaf_node *>(cur);
      size_t i = lower_idx(lf->data, data.first, this);
      if (i < lf->data.size() && eq_key(lf->data[i].first, data.first))
        return {};
      lf->data.insert(lf->data.begin() + static_cast<ptrdiff_t>(i),
                      std::move(data));
      if (lf->data.size() <= max_keys)
        return {};
      auto right = std::make_unique<leaf_node>();
      size_t mid = lf->data.size() / 2;
      right->data.assign(std::make_move_iterator(lf->data.begin() +
                                                 static_cast<ptrdiff_t>(mid)),
                         std::make_move_iterator(lf->data.end()));
      lf->data.erase(lf->data.begin() + static_cast<ptrdiff_t>(mid),
                     lf->data.end());
      right->next = lf->next;
      if (right->next)
        right->next->prev = right.get();
      right->prev = lf;
      lf->next = right.get();
      if (_last_leaf == lf)
        _last_leaf = right.get();
      return {right->data.front().first, std::move(right), true};
    }

    auto *in = static_cast<inner_node *>(cur);
    size_t i = lower_idx_keys(in->keys, data.first, this);
    auto child_split = insert_rec(in->children[i].get(), std::move(data));
    if (!child_split.has)
      return {};
    in->keys.insert(in->keys.begin() + static_cast<ptrdiff_t>(i),
                    child_split.sep);
    in->children.insert(in->children.begin() + static_cast<ptrdiff_t>(i + 1),
                        std::move(child_split.right));
    if (in->keys.size() <= max_keys)
      return {};
    auto right = std::make_unique<inner_node>();
    size_t mid = in->keys.size() / 2;
    tkey sep = in->keys[mid];
    right->keys.assign(std::make_move_iterator(in->keys.begin() +
                                               static_cast<ptrdiff_t>(mid + 1)),
                       std::make_move_iterator(in->keys.end()));
    in->keys.erase(in->keys.begin() + static_cast<ptrdiff_t>(mid),
                   in->keys.end());
    right->children.assign(
        std::make_move_iterator(in->children.begin() +
                                static_cast<ptrdiff_t>(mid + 1)),
        std::make_move_iterator(in->children.end()));
    in->children.erase(in->children.begin() + static_cast<ptrdiff_t>(mid + 1),
                       in->children.end());
    return {sep, std::move(right), true};
  }

  void rebuild_leaf_links() {
    _first_leaf = _last_leaf = nullptr;
    if (!_root)
      return;
    std::vector<leaf_node *> leaves;
    collect_leaves(_root.get(), leaves);
    for (size_t i = 0; i < leaves.size(); ++i) {
      leaves[i]->prev = (i ? leaves[i - 1] : nullptr);
      leaves[i]->next = (i + 1 < leaves.size() ? leaves[i + 1] : nullptr);
    }
    if (!leaves.empty()) {
      _first_leaf = leaves.front();
      _last_leaf = leaves.back();
    }
  }
  void collect_leaves(node_base *n, std::vector<leaf_node *> &out) {
    if (n->is_leaf) {
      out.push_back(static_cast<leaf_node *>(n));
      return;
    }
    auto *in = static_cast<inner_node *>(n);
    for (auto &c : in->children)
      collect_leaves(c.get(), out);
  }

public:
  explicit BP_tree(const compare &cmp = compare(),
                   pp_allocator<value_type> alloc = pp_allocator<value_type>())
      : compare(cmp), _allocator(alloc) {
    static_assert(t > 1);
  }
  explicit BP_tree(pp_allocator<value_type> alloc,
                   const compare &cmp = compare())
      : BP_tree(cmp, alloc) {}
  template <input_iterator_for_pair<tkey, tvalue> iterator>
  explicit BP_tree(iterator begin, iterator end, const compare &cmp = compare(),
                   pp_allocator<value_type> alloc = pp_allocator<value_type>())
      : BP_tree(cmp, alloc) {
    for (auto it = begin; it != end; ++it)
      insert(*it);
  }
  BP_tree(std::initializer_list<std::pair<tkey, tvalue>> data,
          const compare &cmp = compare(),
          pp_allocator<value_type> alloc = pp_allocator<value_type>())
      : BP_tree(data.begin(), data.end(), cmp, alloc) {}

  BP_tree(const BP_tree &other)
      : compare(static_cast<const compare &>(other)),
        _allocator(other._allocator) {
    for (auto it = other.cbegin(); it != other.cend(); ++it)
      insert(*it);
  }
  BP_tree(BP_tree &&other) noexcept = default;
  BP_tree &operator=(const BP_tree &other) {
    if (this == &other)
      return *this;
    clear();
    static_cast<compare &>(*this) = static_cast<const compare &>(other);
    _allocator = other._allocator;
    for (auto it = other.cbegin(); it != other.cend(); ++it)
      insert(*it);
    return *this;
  }
  BP_tree &operator=(BP_tree &&other) noexcept = default;
  ~BP_tree() noexcept = default;

  class bptree_iterator {
    leaf_node *_node = nullptr;
    size_t _index = 0;
    friend class BP_tree;
    friend class bptree_const_iterator;

  public:
    using value_type = tree_data_type_const;
    using reference = tree_data_type &;
    using pointer = tree_data_type *;
    using iterator_category = std::forward_iterator_tag;
    using difference_type = ptrdiff_t;
    using self = bptree_iterator;
    explicit bptree_iterator(leaf_node *n = nullptr, size_t i = 0)
        : _node(n), _index(i) {}
    reference operator*() const noexcept { return _node->data[_index]; }
    pointer operator->() const noexcept { return &_node->data[_index]; }
    self &operator++() {
      if (!_node)
        return *this;
      ++_index;
      if (_index >= _node->data.size()) {
        _node = _node->next;
        _index = 0;
      }
      return *this;
    }
    self operator++(int) {
      auto c = *this;
      ++(*this);
      return c;
    }
    bool operator==(const self &o) const noexcept {
      return _node == o._node && _index == o._index;
    }
    bool operator!=(const self &o) const noexcept { return !(*this == o); }
    size_t current_node_keys_count() const noexcept {
      return _node ? _node->data.size() : 0;
    }
    size_t index() const noexcept { return _index; }
  };
  class bptree_const_iterator {
    const leaf_node *_node = nullptr;
    size_t _index = 0;
    friend class BP_tree;

  public:
    using value_type = tree_data_type_const;
    using reference = const tree_data_type &;
    using pointer = const tree_data_type *;
    using iterator_category = std::forward_iterator_tag;
    using difference_type = ptrdiff_t;
    using self = bptree_const_iterator;
    explicit bptree_const_iterator(const leaf_node *n = nullptr, size_t i = 0)
        : _node(n), _index(i) {}
    bptree_const_iterator(const bptree_iterator &it) noexcept
        : _node(it._node), _index(it._index) {}
    reference operator*() const noexcept { return _node->data[_index]; }
    pointer operator->() const noexcept { return &_node->data[_index]; }
    self &operator++() {
      if (!_node)
        return *this;
      ++_index;
      if (_index >= _node->data.size()) {
        _node = _node->next;
        _index = 0;
      }
      return *this;
    }
    self operator++(int) {
      auto c = *this;
      ++(*this);
      return c;
    }
    bool operator==(const self &o) const noexcept {
      return _node == o._node && _index == o._index;
    }
    bool operator!=(const self &o) const noexcept { return !(*this == o); }
    size_t current_node_keys_count() const noexcept {
      return _node ? _node->data.size() : 0;
    }
    size_t index() const noexcept { return _index; }
  };

  tvalue &at(const tkey &key) {
    auto *lf = find_leaf(key);
    if (!lf)
      throw key_not_found();
    size_t i = lower_idx(lf->data, key, this);
    if (i >= lf->data.size() || !eq_key(lf->data[i].first, key))
      throw key_not_found();
    return lf->data[i].second;
  }
  const tvalue &at(const tkey &key) const {
    return const_cast<BP_tree *>(this)->at(key);
  }
  tvalue &operator[](const tkey &key) {
    auto [it, _] = emplace(key, tvalue{});
    return it->second;
  }
  tvalue &operator[](tkey &&key) {
    auto [it, _] = emplace(std::move(key), tvalue{});
    return it->second;
  }

  bptree_iterator begin() {
    return (!_first_leaf || _first_leaf->data.empty())
               ? end()
               : bptree_iterator(_first_leaf, 0);
  }
  bptree_iterator end() { return bptree_iterator(nullptr, 0); }
  bptree_const_iterator begin() const { return cbegin(); }
  bptree_const_iterator end() const { return cend(); }
  bptree_const_iterator cbegin() const {
    return (!_first_leaf || _first_leaf->data.empty())
               ? cend()
               : bptree_const_iterator(_first_leaf, 0);
  }
  bptree_const_iterator cend() const {
    return bptree_const_iterator(nullptr, 0);
  }

  size_t size() const noexcept { return _size; }
  bool empty() const noexcept { return _size == 0; }
  bptree_iterator find(const tkey &key) {
    auto *lf = find_leaf(key);
    if (!lf)
      return end();
    size_t i = lower_idx(lf->data, key, this);
    return (i < lf->data.size() && eq_key(lf->data[i].first, key))
               ? bptree_iterator(lf, i)
               : end();
  }
  bptree_const_iterator find(const tkey &key) const {
    auto it = const_cast<BP_tree *>(this)->find(key);
    return bptree_const_iterator(it);
  }
  bptree_iterator lower_bound(const tkey &key) {
    auto *lf = find_leaf(key);
    if (!lf)
      return end();
    size_t i = lower_idx(lf->data, key, this);
    if (i < lf->data.size())
      return bptree_iterator(lf, i);
    lf = lf->next;
    return lf ? bptree_iterator(lf, 0) : end();
  }
  bptree_const_iterator lower_bound(const tkey &key) const {
    return bptree_const_iterator(const_cast<BP_tree *>(this)->lower_bound(key));
  }
  bptree_iterator upper_bound(const tkey &key) {
    auto it = lower_bound(key);
    if (it != end() && eq_key(it->first, key))
      ++it;
    return it;
  }
  bptree_const_iterator upper_bound(const tkey &key) const {
    return bptree_const_iterator(const_cast<BP_tree *>(this)->upper_bound(key));
  }
  bool contains(const tkey &key) const { return find(key) != cend(); }

  void clear() noexcept {
    _root.reset();
    _first_leaf = nullptr;
    _last_leaf = nullptr;
    _size = 0;
  }
  std::pair<bptree_iterator, bool> insert(const tree_data_type &data) {
    return emplace(data.first, data.second);
  }
  std::pair<bptree_iterator, bool> insert(tree_data_type &&data) {
    return emplace(std::move(data.first), std::move(data.second));
  }
  template <typename... Args>
  std::pair<bptree_iterator, bool> emplace(Args &&...args) {
    tree_data_type data(std::forward<Args>(args)...);
    auto ex = find(data.first);
    if (ex != end())
      return {ex, false};
    if (!_root) {
      auto lf = std::make_unique<leaf_node>();
      _first_leaf = _last_leaf = lf.get();
      _root = std::move(lf);
    }
    const tkey inserted_key = data.first;
    auto sp = insert_rec(_root.get(), std::move(data));
    if (sp.has) {
      auto nr = std::make_unique<inner_node>();
      nr->keys.push_back(sp.sep);
      nr->children.push_back(std::move(_root));
      nr->children.push_back(std::move(sp.right));
      _root = std::move(nr);
    }
    ++_size;
    return {find(inserted_key), true};
  }
  bptree_iterator erase(const tkey &key) {
    auto it = find(key);
    if (it == end())
      return end();
    auto *lf = it._node;
    lf->data.erase(lf->data.begin() + static_cast<ptrdiff_t>(it._index));
    --_size;
    if (_size == 0) {
      clear();
      return end();
    }
    if (lf->data.empty())
      rebuild_leaf_links();
    return lower_bound(key);
  }
  bptree_iterator erase(bptree_iterator pos) {
    return pos == end() ? end() : erase(pos->first);
  }
  bptree_iterator erase(bptree_const_iterator pos) {
    return pos == cend() ? end() : erase(pos->first);
  }
  bptree_iterator erase(bptree_const_iterator beg, bptree_const_iterator en) {
    while (beg != en) {
      auto k = beg->first;
      ++beg;
      erase(k);
    }
    return end();
  }

  pp_allocator<value_type> get_allocator() const noexcept { return _allocator; }
};

#endif
