module;
export module graph.impl;
import std;
export import :basis;
export import :spmat;

export namespace graph {

// ---------- 3) DirectedGraph ----------
template <class Key, class NodeW = void, class EdgeW = void>
class DirectedGraph {
public:
  using id_type = ::graph::id_type;

private:
  struct Edge : WeightMixin<EdgeW> {
    id_type to{};
    using WM = WeightMixin<EdgeW>;

    explicit Edge(id_type t) : WM(), to(t) {}

    template <class W = EdgeW>
    Edge(id_type t, const W &w)
      requires Weighted<W>
        : WM(w), to(t) {}

    template <class W = EdgeW>
    Edge(id_type t, W &&w)
      requires Weighted<W>
        : WM(std::move(w)), to(t) {}
  };

  struct NodeRec : WeightMixin<NodeW> {
    Key key;
    std::vector<Edge> out;
    std::vector<id_type> in;
    using WM = WeightMixin<NodeW>;

    explicit NodeRec(Key k) : WM(), key(std::move(k)), out() {}

    template <class W = NodeW>
    NodeRec(Key k, const W &w)
      requires Weighted<W>
        : WM(w), key(std::move(k)), out() {}

    template <class W = NodeW>
    NodeRec(Key k, W &&w)
      requires Weighted<W>
        : WM(std::move(w)), key(std::move(k)), out() {}
  };

  std::vector<NodeRec> nodes_;
  std::unordered_map<Key, id_type> id_of_;

public:
  // ---------- 节点 ----------
  id_type add_node(const Key &key)
    requires(std::is_void_v<NodeW> || std::is_default_constructible_v<NodeW>)
  {
    if (auto it = id_of_.find(key); it != id_of_.end())
      return it->second;
    id_type id = nodes_.size();
    nodes_.emplace_back(key);
    try {
      id_of_.emplace(key, id);
    } catch (...) {
      nodes_.pop_back();
      throw;
    }
    return id;
  }

  template <class W = NodeW>
  id_type add_node(const Key &key, const W &w)
    requires Weighted<W>
  {
    if (auto it = id_of_.find(key); it != id_of_.end())
      return it->second;
    id_type id = nodes_.size();
    nodes_.emplace_back(key, w);
    try {
      id_of_.emplace(key, id);
    } catch (...) {
      nodes_.pop_back();
      throw;
    }
    return id;
  }

  // ---------- 边 ----------
  void add_edge(const Key &from, const Key &to)
    requires(std::is_void_v<EdgeW> || std::is_default_constructible_v<EdgeW>)
  {
    id_type u = add_node(from);
    id_type v = add_node(to);
    nodes_[u].out.emplace_back(v);
    try {
      nodes_[v].in.push_back(u);
    } catch (...) {
      nodes_[u].out.pop_back();
      throw;
    }
  }

  template <class W = EdgeW>
  void add_edge(const Key &from, const Key &to, const W &w)
    requires Weighted<W>
  {
    id_type u = add_node(from);
    id_type v = add_node(to);
    nodes_[u].out.emplace_back(v, w);
    try {
      nodes_[v].in.push_back(u);
    } catch (...) {
      nodes_[u].out.pop_back();
      throw;
    }
  }

  // ---------- 查询 ----------
  bool has_node(const Key &key) const { return id_of_.contains(key); }

  id_type id(const Key &key) const {
    auto it = id_of_.find(key);
    if (it == id_of_.end())
      throw std::invalid_argument("node key not found");
    return it->second;
  }

  // ---------- 删除 ----------
  bool remove_edge(const Key &from, const Key &to) {
    if (!id_of_.contains(from) || !id_of_.contains(to))
      return false;

    id_type u = id_of_[from];
    id_type v = id_of_[to];

    auto &edges = nodes_[u].out;
    auto it = std::remove_if(edges.begin(), edges.end(),
                             [v](const Edge &e) { return e.to == v; });

    if (it != edges.end()) {
      edges.erase(it, edges.end());
      auto &in_edges = nodes_[v].in;
      std::erase(in_edges, u);
      return true;
    }
    return false;
  }

  // Removing a node may change the last node's ID. Keys remain stable.
  bool remove_node(const Key &key) {
    auto it = id_of_.find(key);
    if (it == id_of_.end()) return false;
    const id_type removed = it->second;
    const id_type last = nodes_.size() - 1;

    // Every outgoing edge must have a matching incoming entry, including duplicates.
    for (auto &node : nodes_) {
      std::erase_if(node.out, [removed](const Edge &edge) { return edge.to == removed; });
      std::erase(node.in, removed);
      if (removed != last) {
        for (auto &edge : node.out) if (edge.to == last) edge.to = removed;
        for (auto &source : node.in) if (source == last) source = removed;
      }
    }
    id_of_.erase(it);
    if (removed != last) {
      nodes_[removed] = std::move(nodes_[last]);
      id_of_.at(nodes_[removed].key) = removed;
    }
    nodes_.pop_back();
    return true;
  }

  // ---------- 查询 ----------
  const std::vector<Edge> &out_edges(id_type id) const {
    return nodes_.at(id).out;
  }

  const std::vector<id_type> &in_edges(id_type id) const {
    return nodes_.at(id).in;
  }

  id_type out_degree(id_type id) const {
    return nodes_.at(id).out.size();
  }

  id_type in_degree(id_type id) const {
    return nodes_.at(id).in.size();
  }

  id_type degree(id_type id) const {
    return out_degree(id) + in_degree(id);
  }

  bool is_valid(id_type id) const { return id < nodes_.size(); }

  id_type node_count() const { return nodes_.size(); }
  const Key &key(id_type id) const { return nodes_.at(id).key; }

  // CSR build
  CSR<EdgeW> build_csr(bool sort_by_to = true) const {
    const id_type n = nodes_.size();

    CSR<EdgeW> csr;
    csr.num_rows = n;
    csr.num_cols = n;

    csr.indptr.resize(n + 1);
    csr.indptr[0] = 0;

    for (id_type i = 0; i < n; ++i) {
      csr.indptr[i + 1] = nodes_[i].out.size();
    }

    std::inclusive_scan(csr.indptr.begin(), csr.indptr.end(), csr.indptr.begin());

    const id_type nnz = csr.indptr[n];
    csr.indices.resize(nnz);
    if constexpr (Weighted<EdgeW>)
      csr.data.resize(nnz);

    std::conditional_t<Weighted<EdgeW>, std::vector<std::pair<id_type, EdgeW>>, WeightBox<void>> sort_buf;
    if constexpr (Weighted<EdgeW>) {
      if (sort_by_to)
        sort_buf.reserve(16);
    }

    for (id_type i = 0; i < n; ++i) {
      const auto &src_edges = nodes_[i].out;
      const id_type start = csr.indptr[i];
      const id_type count = src_edges.size();

      for (id_type k = 0; k < count; ++k) {
        csr.indices[start + k] = src_edges[k].to;

        if constexpr (Weighted<EdgeW>) {
          csr.data[start + k] = src_edges[k].weight();
        }
      }

      if (sort_by_to && count > 1) {
        if constexpr (!Weighted<EdgeW>) {
          std::sort(csr.indices.begin() + start,
                    csr.indices.begin() + start + count);
        } else {
          sort_buf.clear();
          for (id_type k = 0; k < count; ++k) {
            sort_buf.emplace_back(csr.indices[start + k], csr.data[start + k]);
          }

          std::sort(
              sort_buf.begin(), sort_buf.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

          for (id_type k = 0; k < count; ++k) {
            csr.indices[start + k] = sort_buf[k].first;
            csr.data[start + k] = sort_buf[k].second;
          }
        }
      }
    }
    return csr;
  }

  // CSC build
  CSC<EdgeW> build_csc(bool sort_by_from = true) const {
    const id_type n = nodes_.size();
    CSC<EdgeW> csc;
    csc.num_rows = n;
    csc.num_cols = n;

    csc.indptr.assign(n + 1, 0);

    for (const auto &node : nodes_) {
      for (const auto &e : node.out) {
        if (e.to < n)
          csc.indptr[e.to + 1]++;
      }
    }

    std::inclusive_scan(csc.indptr.begin(), csc.indptr.end(), csc.indptr.begin());

    const id_type nnz = csc.indptr.back();
    csc.indices.resize(nnz);
    if constexpr (Weighted<EdgeW>)
      csc.data.resize(nnz);

    std::vector<id_type> cursor = csc.indptr;

    for (id_type u = 0; u < n; ++u) {
      for (const auto &e : nodes_[u].out) {
        if (e.to >= n)
          continue;

        id_type pos = cursor[e.to]++;

        csc.indices[pos] = u;
        if constexpr (Weighted<EdgeW>) {
          csc.data[pos] = e.weight();
        }
      }
    }

    if (sort_by_from) {
      std::conditional_t<Weighted<EdgeW>, std::vector<std::pair<id_type, EdgeW>>, WeightBox<void>> tmp;
      if constexpr (Weighted<EdgeW>)
        tmp.reserve(16);

      for (id_type j = 0; j < n; ++j) {
        id_type start = csc.indptr[j];
        id_type end = csc.indptr[j + 1];
        id_type len = end - start;

        if (len <= 1)
          continue;

        if constexpr (!Weighted<EdgeW>) {
          std::sort(csc.indices.begin() + start, csc.indices.begin() + end);
        } else {
          tmp.clear();
          for (id_type k = 0; k < len; ++k) {
            tmp.emplace_back(csc.indices[start + k], csc.data[start + k]);
          }
          std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
          });

          for (id_type k = 0; k < len; ++k) {
            csc.indices[start + k] = tmp[k].first;
            csc.data[start + k] = tmp[k].second;
          }
        }
      }
    }

    return csc;
  }

  void reserve_nodes(id_type count) {
    nodes_.reserve(count);
    id_of_.reserve(count);
  }
  void reserve_out_edges(id_type node, id_type count) { nodes_.at(node).out.reserve(count); }

  // Keep the induced subgraph, preserving node order. Does not bypass removed nodes.
  // Evaluate predicates before mutation so a predicate exception leaves the graph intact.
  template <class Predicate> void retain_nodes(Predicate &&should_keep) {
    constexpr id_type removed = std::numeric_limits<id_type>::max();
    std::vector<id_type> remap(nodes_.size(), removed);
    id_type count = 0;
    for (id_type old = 0; old < nodes_.size(); ++old) {
      if (should_keep(nodes_[old].key)) remap[old] = count++;
    }
    for (id_type old = 0; old < nodes_.size(); ++old) {
      if (remap[old] != removed && remap[old] != old)
        nodes_[remap[old]] = std::move(nodes_[old]);
    }
    nodes_.erase(nodes_.begin() + count, nodes_.end());
    for (auto &node : nodes_) {
      std::erase_if(node.out, [&](const Edge &edge) { return remap[edge.to] == removed; });
      std::erase_if(node.in, [&](id_type source) { return remap[source] == removed; });
      for (auto &edge : node.out) edge.to = remap[edge.to];
      for (auto &source : node.in) source = remap[source];
    }
    id_of_.clear();
    for (id_type id = 0; id < nodes_.size(); ++id) id_of_.emplace(nodes_[id].key, id);
  }

};

} // namespace graph
