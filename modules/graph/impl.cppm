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
  using id_type = id_type;

private:
  struct Edge : WeightMixin<EdgeW> {
    id_type to{};
    using WM = WeightMixin<EdgeW>;

    // 无权重构造：永远存在
    explicit Edge(id_type t) : WM(), to(t) {}

    // 带权构造：做成模板，让 void 不会出现在签名里
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
    std::vector<id_type> in; // 入边 (别人 -> 我) [新增!]
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
    id_of_.emplace(key, id);
    return id;
  }

  // 带权 add_node：模板签名，void 时自动被丢掉
  template <class W = NodeW>
  id_type add_node(const Key &key, const W &w)
    requires Weighted<W>
  {
    if (auto it = id_of_.find(key); it != id_of_.end())
      return it->second;
    id_type id = nodes_.size();
    nodes_.emplace_back(key, w);
    id_of_.emplace(key, id);
    return id;
  }

  // ---------- 边 ----------
  void add_edge(const Key &from, const Key &to)
    requires(std::is_void_v<EdgeW> || std::is_default_constructible_v<EdgeW>)
  {
    id_type u = add_node(from);
    id_type v = add_node(to);
    nodes_[u].out.emplace_back(v);
    nodes_[v].in.push_back(u);
  }

  template <class W = EdgeW>
  void add_edge(const Key &from, const Key &to, const W &w)
    requires Weighted<W>
  {
    id_type u = add_node(from);
    id_type v = add_node(to);
    nodes_[u].out.emplace_back(v, w);
  }

  // ---------- 查询 ----------
  bool has_node(const Key &key) const { return id_of_.contains(key); }

  id_type id(const Key &key) const {
    auto it = id_of_.find(key);
    if (it == id_of_.end())
      throw std::invalid_argument("node key not found");
    return it->second;
  }
  // ========================================================
  // 新增：删除功能
  // ========================================================

  // 1. 删除边 (O(OutDegree)) - 物理删除
  bool remove_edge(const Key &from, const Key &to) {
    if (!id_of_.contains(from) || !id_of_.contains(to))
      return false;

    id_type u = id_of_[from];
    id_type v = id_of_[to]; // 目标 ID

    auto &edges = nodes_[u].out;

    // C++20 std::erase_if (或者 remove_if + erase)
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
  // 删除节点 (O(V + E))
  // 逻辑：将末尾节点移到被删除的位置，填补空缺
  bool remove_node(const Key &key) {
    auto it = id_of_.find(key);
    if (it == id_of_.end())
      return false;

    id_type target_id = it->second;      // 要删除的坑位
    id_type last_id = nodes_.size() - 1; // 末尾的人

    // 1. 从 Map 移除自己
    id_of_.erase(it);

    // 2. 如果要删的不是最后一个，需要做搬迁工作
    if (target_id != last_id) {
      // A. 全图修正边 (最耗时的一步)
      // 我们需要做两件事：
      //   1. 删掉指向 target_id 的边
      //   2. 把指向 last_id 的边，重定向到 target_id
      for (auto &node : nodes_) {
        auto &edges = node.out;

        // 使用 erase_if 结合 lambda 进行修改是 C++ 的一种常见模式
        // 但为了逻辑清晰，我们分两步，或者在 remove_if 里做 side-effect
        auto edge_it = std::remove_if(edges.begin(), edges.end(), [&](Edge &e) {
          if (e.to == target_id)
            return true; // 删掉指向死者的边
          if (e.to == last_id)
            e.to = target_id; // 修正指向搬迁者的边
          return false;
        });
        edges.erase(edge_it, edges.end());
      }

      // B. 搬迁数据 (Last -> Target)
      nodes_[target_id] = std::move(nodes_[last_id]);

      // C. 更新 Map 映射
      // 刚才搬过来的那个节点，它的 Key 还在，但 ID 变了
      const Key &moved_key = nodes_[target_id].key;
      id_of_[moved_key] = target_id;

    } else {
      // 3. 如果恰好删的是最后一个，只需要清理指向它的边
      for (auto &node : nodes_) {
        auto &edges = node.out;
        std::erase_if(edges,
                      [target_id](const Edge &e) { return e.to == target_id; });
      }
    }

    // 4. 物理弹出
    nodes_.pop_back();

    return true;
  }

  // ----------------------------------------------------------
  // 新增：出边查询 (Predecessors)
  // ----------------------------------------------------------

  const std::vector<Edge> &out_edges(id_type id) const {
    return nodes_.at(id).out;
  }

  // ----------------------------------------------------------
  // 新增：入边查询 (Predecessors)
  // ----------------------------------------------------------
  const std::vector<id_type> &in_edges(id_type id) const {
    // 利用了 NodeRec 中的 in 数组
    return nodes_.at(id).in;
  }

  // ----------------------------------------------------------
  // 新增：度数查询 (Degree Access)
  // ----------------------------------------------------------

  // 1. 出度：O(1)
  id_type out_degree(id_type id) const {
    // 直接返回 out 向量的长度
    return nodes_.at(id).out.size();
  }

  // 2. 入度：O(1)
  id_type in_degree(id_type id) const {
    // 直接返回 in 向量的长度
    return nodes_.at(id).in.size();
  }

  // 3. 总度数：O(1)
  id_type degree(id_type id) const {
    // 入度 + 出度
    return out_degree(id) + in_degree(id);
  }
  // ========================================================
  // 迭代器修正
  // ========================================================

  bool is_valid(id_type id) const { return id < nodes_.size(); }

  id_type node_count() const { return nodes_.size(); }
  const Key &key(id_type id) const { return nodes_.at(id).key; }

  // CSR/CSC build 部分你原样搬过来即可（只把访问 weight 的地方留在 if constexpr
  // 里）
  // ---------- CSR / CSC ----------
  // CSR: 行 = from（出边）
  CSR<EdgeW> build_csr(bool sort_by_to = true) const {
    const id_type n = nodes_.size();

    // 1. 计算总边数 (Pass 1)
    // 我们可以直接 resize indptr，然后用 exclusive_scan 计算前缀和
    // 但为了求 indices 的大小，我们得先算好
    CSR<EdgeW> csr;
    csr.num_rows = n;
    csr.num_cols = 0; // 稍后统计或由外部传入，这里暂存 0

    csr.indptr.resize(n + 1);
    csr.indptr[0] = 0;

    // 直接写入长度
    for (id_type i = 0; i < n; ++i) {
      csr.indptr[i + 1] = nodes_[i].out.size();
    }

    // 前缀和：[2, 3, 1] -> [0, 2, 5, 6]
    // 这一步之后，indptr[i] 就是第 i 行在 indices 中的起始位置
    std::exclusive_scan(csr.indptr.begin(), csr.indptr.end(),
                        csr.indptr.begin(), 0);

    const id_type nnz = csr.indptr[n];
    csr.indices.resize(nnz);
    if constexpr (Weighted<EdgeW>)
      csr.data.resize(nnz);

    // 2. 填充数据 (Pass 2) + 原地排序
    // 预分配一个小的临时 buffer 用于带权排序，避免循环内反复 malloc
    // 只有带权且需要排序时才需要这个
    std::vector<std::pair<id_type, EdgeW>> sort_buf;
    if constexpr (Weighted<EdgeW>) {
      if (sort_by_to)
        sort_buf.reserve(16); // 预估平均度数
    }

    for (id_type i = 0; i < n; ++i) {
      const auto &src_edges = nodes_[i].out; // ✨ 引用！不拷贝！
      const id_type start = csr.indptr[i];
      const id_type count = src_edges.size();

      // 2.1 直接拷贝到 CSR 数组 (此时可能是乱序)
      for (id_type k = 0; k < count; ++k) {
        csr.indices[start + k] = src_edges[k].to;
        // 顺便统计最大列号
        if (src_edges[k].to >= csr.num_cols)
          csr.num_cols = src_edges[k].to + 1;

        if constexpr (Weighted<EdgeW>) {
          csr.data[start + k] = src_edges[k].weight();
        }
      }

      // 2.2 局部排序 (Local Sort)
      if (sort_by_to && count > 1) {
        if constexpr (!Weighted<EdgeW>) {
          // 无权图：直接对 indices 切片排序，超级快！
          std::sort(csr.indices.begin() + start,
                    csr.indices.begin() + start + count);
        } else {
          // 有权图：必须同步排序 indices 和 data
          // 为了代码健壮性，这里把数据拷出来排完再塞回去 (因为 C++ 没有
          // zip_sort) 由于 sort_buf 内存是复用的，这里几乎没有分配开销
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

  // CSC: 列 = to（入边）
  CSC<EdgeW> build_csc(bool sort_by_from = true) const {
    const id_type n = nodes_.size();
    CSC<EdgeW> csc;
    csc.num_rows = n;
    csc.num_cols = n; // 假设是方阵，或者遍历算最大ID

    // 1. 直方图统计 (Histogram)
    // CSC 的 indptr 长度是 cols + 1，这里假设 col_max < n，
    // 如果 col ID 可能比 n 大，需要先遍历一遍找 max_col
    csc.indptr.assign(n + 1, 0);

    for (const auto &node : nodes_) {
      for (const auto &e : node.out) {
        // 假设 e.to < n，如果越界需要扩容 indptr
        if (e.to < n)
          csc.indptr[e.to + 1]++;
      }
    }

    // 2. 前缀和 (Prefix Sum)
    std::exclusive_scan(csc.indptr.begin(), csc.indptr.end(),
                        csc.indptr.begin(), 0);

    const id_type nnz = csc.indptr.back();
    csc.indices.resize(nnz);
    if constexpr (Weighted<EdgeW>)
      csc.data.resize(nnz);

    // 3. 填充 (Bucketing)
    // 我们需要一个 cursor 数组来记录当前列填到哪里了
    std::vector<id_type> cursor = csc.indptr;

    for (id_type u = 0; u < n; ++u) {
      for (const auto &e : nodes_[u].out) {
        if (e.to >= n)
          continue; // 忽略越界边

        id_type pos = cursor[e.to]++; // 获取当前列的写入位置，并后移

        csc.indices[pos] = u; // CSC 存的是行号 (Row Index)
        if constexpr (Weighted<EdgeW>) {
          csc.data[pos] = e.weight();
        }
      }
    }

    // 4. 列内排序 (Sorting)
    if (sort_by_from) {
      // ✨ 优化：内存外提 (Allocation Hoisting)
      // 只有带权图需要这个复杂的 buffer，无权图可以直接 sort indices
      std::vector<std::pair<id_type, EdgeW>> tmp;
      if constexpr (Weighted<EdgeW>)
        tmp.reserve(16);

      for (id_type j = 0; j < n; ++j) {
        id_type start = csc.indptr[j];
        id_type end = csc.indptr[j + 1];
        id_type len = end - start;

        if (len <= 1)
          continue;

        if constexpr (!Weighted<EdgeW>) {
          // 无权：直接排 indices，原地起飞
          std::sort(csc.indices.begin() + start, csc.indices.begin() + end);
        } else {
          // 有权：使用外提的 tmp buffer
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

  // ---------- 预留 ----------
  void reserve_nodes(id_type n) { nodes_.reserve(n); }
  void reserve_out_edges(id_type id, id_type m) {
    nodes_.at(id).out.reserve(m);
  }

  // ========================================================
  // 核心接口：批量清洗 (Retain Nodes)
  // 语义：保留那些 Predicate 返回 true 的节点，清理其他的，并重连所有边
  // 复杂度：O(V + E)
  // ========================================================
  template <typename Predicate> void retain_nodes(Predicate &&should_keep) {
    if (nodes_.empty())
      return;

    const std::size_t old_size = nodes_.size();

    // 1. 建立重映射表 (Old ID -> New ID)
    // 使用 max_value 标记被删除的节点
    constexpr id_type REMOVED = std::numeric_limits<id_type>::max();
    std::vector<id_type> remap(old_size, REMOVED);

    // 2. 压缩节点 (Compacting)
    // 使用 "双指针" 法原地压缩，避免分配新 vector
    id_type new_cursor = 0;

    for (id_type old_i = 0; old_i < old_size; ++old_i) {
      // 这里的 Key 传给用户判断 (比如判断是不是 NoOp)
      if (should_keep(nodes_[old_i].key)) {
        remap[old_i] = new_cursor;

        if (old_i != new_cursor) {
          // 移动语义：把后面的活节点搬到前面
          nodes_[new_cursor] = std::move(nodes_[old_i]);
        }
        new_cursor++;
      }
    }

    // 3. 截断 vector
    nodes_.resize(new_cursor);

    // 4. 重连边 (Rewire Edges)
    for (auto &node : nodes_) {
      auto &edges = node.out;

      // A. 移除指向"已删节点"的边
      auto it = std::remove_if(edges.begin(), edges.end(), [&](const Edge &e) {
        return remap[e.to] == REMOVED;
      });
      edges.erase(it, edges.end());

      // B. 更新指向"存活节点"的边的 ID
      for (auto &edge : edges) {
        edge.to = remap[edge.to];
      }
    }

    // 5. 重建 Map 索引 (Rebuild Index)
    // 因为大量的 ID 都变了，重建比逐个更新更快
    id_of_.clear();
    for (id_type i = 0; i < nodes_.size(); ++i) {
      id_of_.emplace(nodes_[i].key, i);
    }
  }
};

} // namespace graph