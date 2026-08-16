module;
export module graph.algo;
import std;
import graph; // 假设之前的 DirectedGraph 在这里
import graph.traverse;

export namespace graph::algo {
using size_t = std::size_t;
// ----------------------------------------------------------
// 穿透透明节点的可达性遍历 (使用 Version Tagging 优化 Reset)
// ----------------------------------------------------------
template <graph::traverse::IdOutGraph G, class TransparentPred, class Visitor>
void reach_through_transparent(const G &graph, std::size_t start,
                               TransparentPred &&is_transparent,
                               Visitor &&visitor) {
  const auto n = graph.node_count();
  if (start >= n)
    throw std::out_of_range("start id out of range");

  // ⚠️ 优化：使用 size_t 而不是 unsigned char，方便 Version Tagging
  // 尽管分配仍然发生，但我们避免了 O(N) 的显式 memset/fill
  std::vector<std::size_t> seen(n, 0);
  std::size_t current_token = 1; // 引入版本标记
  std::vector<std::size_t> stack;
  stack.reserve(n);

  for (const auto &e : graph.out_edges(start)) {
    stack.push_back(e.to);
  }

  while (!stack.empty()) {
    auto u = stack.back();
    stack.pop_back();

    if (u >= n || seen[u] == current_token) // 使用 token 检查是否已访问
      continue;

    seen[u] = current_token; // 标记为当前版本已访问

    if (!is_transparent(u)) {
      visitor(u);
      continue;
    }
    for (const auto &e : graph.out_edges(u)) {
      stack.push_back(e.to);
    }
  }
}

// -------------------------------------------------------------------------
// Tarjan's SCC 结果的内存优化结构 (Flat Array)
// -------------------------------------------------------------------------
struct SCCFlatResult {
  std::vector<int> component_ids; // component_ids[node] = SCC ID
  int scc_count = 0;
  bool has_cycle = false;
};
// -------------------------------------------------------------------------
// 1. Tarjan's SCC (返回 Flat Array 结果)
// -------------------------------------------------------------------------
template <typename G> SCCFlatResult tarjan_scc(const G &graph) {
  using size_t = std::size_t;
  size_t n = graph.node_count();
  std::vector<int> dfn(n, -1), low(n, -1);
  std::vector<bool> in_stack(n, false);
  std::stack<size_t> st;
  int timer = 0;
  SCCFlatResult res;
  res.component_ids.assign(n, -1);

  auto dfs = [&](auto &&self, size_t u) -> void {
    dfn[u] = low[u] = ++timer;
    st.push(u);
    in_stack[u] = true;

    for (const auto &edge : graph.out_edges(u)) {
      size_t v = edge.to;
      if (dfn[v] == -1) {
        self(self, v);
        low[u] = std::min(low[u], low[v]);
      } else if (in_stack[v]) {
        low[u] = std::min(low[u], dfn[v]);
      }
    }

    if (low[u] == dfn[u]) {
      size_t component_size = 0;
      while (true) {
        size_t v = st.top();
        st.pop();
        in_stack[v] = false;
        res.component_ids[v] = res.scc_count;
        component_size++;
        if (u == v)
          break;
      }
      if (component_size > 1)
        res.has_cycle = true;
      res.scc_count++;
    }
  };

  for (size_t i = 0; i < n; ++i) {
    if (dfn[i] == -1)
      dfs(dfs, i);
  }

  return res;
}
// ----------------------------------------------------------
// 实用工具：将平坦的 SCC ID 映射重新组织成分组列表 (O(N) 转换)
// ----------------------------------------------------------
template <typename IdType>
std::vector<std::vector<IdType>>
group_sccs(const std::vector<int> &component_ids, int scc_count) {
  if (scc_count <= 0)
    return {};

  std::vector<std::vector<IdType>> components(scc_count);

  for (IdType node_id = 0; node_id < component_ids.size(); ++node_id) {
    int scc_id = component_ids[node_id];
    if (scc_id != -1) {
      components[scc_id].push_back(node_id);
    }
  }
  return components;
}

// ----------------------------------------------------------
// I. 辅助结构：LayerRange (一个层级的节点视图)
// ----------------------------------------------------------
// 它的 begin/end 就是 span 的 begin/end，代表一个可迭代的节点列表
struct LayerRange {
  std::span<const std::size_t> nodes;

  auto begin() const { return nodes.begin(); }
  auto end() const { return nodes.end(); }
};

// -------------------------------------------------------------------------
// 结果结构：用于返回扁平化的层级信息 (类似 CSR/COO 格式)
// -------------------------------------------------------------------------
struct KahnResult {
  // 存储所有节点ID，按拓扑层级顺序排列 (相当于 CSR 的 indices)
  std::vector<std::size_t> nodes_sorted;

  // 存储每层结束的位置 (相当于 CSR 的 indptr)
  std::vector<std::size_t> layer_offsets;

  bool has_cycle = false;

  // ----------------------------------------------------------
  // II. 迭代器：LayerIterator (在外层迭代)
  // ----------------------------------------------------------
  struct LayerIterator {
    const KahnResult *result;
    std::size_t current_layer_index; // 存储当前层级在 offsets 数组中的索引

    // 迭代器类型定义 (C++ 标准要求)
    using iterator_category = std::forward_iterator_tag;
    using value_type = LayerRange;
    using difference_type = std::ptrdiff_t;

    // *操作符：返回当前层级的 std::span 视图
    LayerRange operator*() const {
      std::size_t start_offset = result->layer_offsets[current_layer_index];
      std::size_t end_offset = result->layer_offsets[current_layer_index + 1];

      return {std::span<const std::size_t>{result->nodes_sorted.data() +
                                               start_offset,
                                           end_offset - start_offset}};
    }

    // 前置递增 (Prefix increment): ++it
    LayerIterator &operator++() {
      ++current_layer_index;
      return *this;
    }

    // 比较操作符 (用于判断是否到达 end())
    bool operator==(const LayerIterator &other) const {
      return current_layer_index == other.current_layer_index;
    }
    bool operator!=(const LayerIterator &other) const {
      return !(*this == other);
    }
  };

  // ----------------------------------------------------------
  // III. KahnResult 容器接口 (添加 begin/end 方法)
  // ----------------------------------------------------------

  // 请将以下内容添加到 struct KahnResult 的 public 区域：

  LayerIterator begin() const {
    // 迭代器从第 0 层开始
    return {this, 0};
  }

  LayerIterator end() const {
    // 迭代器指向最后一层之后的位置。
    // layer_offsets 的 size 是 (LayerCount + 1)，所以 end() 索引就是 size() -
    // 1。
    return {this, layer_offsets.size() - 1};
  }
};

// -------------------------------------------------------------------------
// 2. Kahn's Algorithm Layering (返回扁平化结构，消除 vector<vector> 内存碎片)
// -------------------------------------------------------------------------
template <typename G> KahnResult kahn_layers(const G &graph) {
  using size_t = std::size_t;
  size_t n = graph.node_count();

  // ... (入度计算 O(N) 部分略) ...
  std::vector<size_t> in_degree(n);
  for (size_t i = 0; i < n; ++i) {
    in_degree[i] = graph.in_degree(i);
  }
  std::queue<size_t> q;
  for (size_t i = 0; i < n; ++i) {
    if (in_degree[i] == 0)
      q.push(i);
  }

  KahnResult res;
  res.layer_offsets.push_back(0); //  ~！Layer 0 起始于 nodes_sorted[0]
  size_t processed_count = 0;

  while (!q.empty()) {
    size_t layer_size = q.size();

    // ✨ 优化：提前为当前层的节点预留空间，避免 realloc
    res.nodes_sorted.reserve(res.nodes_sorted.size() + layer_size);

    // 3. 处理当前层的所有节点
    for (size_t i = 0; i < layer_size; ++i) {
      size_t u = q.front();
      q.pop();

      res.nodes_sorted.push_back(u); // ✨ 直接写入连续内存
      processed_count++;

      // 减少邻居入度
      for (const auto &edge : graph.out_edges(u)) {
        size_t v = edge.to;
        in_degree[v]--;
        if (in_degree[v] == 0)
          q.push(v);
      }
    }

    // 记录当前层结束的位置 (Layer Offsets)
    res.layer_offsets.push_back(res.nodes_sorted.size());
  }

  // 4. 环检测
  if (processed_count != n) {
    res.has_cycle = true;
  }

  return res;
}

// ----------------------------------------------------------
// 实用工具：重新实现 (利用双向图)
// ----------------------------------------------------------

// 1. 整图入口节点（sources）：入度为 0
template <typename G>
std::vector<typename G::id_type> inbound_nodes(const G &graph) {
  using id_type = typename G::id_type;
  const size_t n = graph.node_count();
  std::vector<id_type> result;
  result.reserve(n);

  for (size_t i = 0; i < n; ++i) {
    // 假设 G::in_degree(i) 是 O(1) 的 API
    if (graph.in_degree(i) == 0) {
      result.push_back(static_cast<id_type>(i));
    }
  }
  return result;
}

// 2. 整图出口节点（sinks）：出度为 0
template <typename G>
std::vector<typename G::id_type> outbound_nodes(const G &graph) {
  using id_type = typename G::id_type;
  const size_t n = graph.node_count();
  std::vector<id_type> result;
  result.reserve(n);

  for (size_t u = 0; u < n; ++u) {
    if (graph.out_edges(u).empty()) { // 利用 out_edges().empty()
      result.push_back(static_cast<id_type>(u));
    }
  }
  return result;
}

// 3. 单节点前驱集合 (predecessors)
// O(deg_in) 复杂度，利用双向图的 in_edges 接口
template <typename G>
std::vector<typename G::id_type> predecessors(const G &graph,
                                              typename G::id_type v) {
  using id_type = typename G::id_type;

  // ⚠️ 假设 G 满足：graph.in_edges(v) -> const vector<id_type>& (或类似)
  // 如果 graph.in_edges() 返回的是 vector<id_type>，则直接返回即可
  // 如果返回的是一个 range 或 vector<id_type>，这里需要一次拷贝或适配

  // 由于我们已经修改了 DirectedGraph，这里直接假设存在 in_edges 接口
  // 并且返回 vector<id_type>，需要拷贝
  std::vector<id_type> preds;
  for (const auto &u : graph.in_edges(v)) {
    preds.push_back(u);
  }
  return preds;
}

// 4. 单节点后继集合 (successors)
// O(deg_out) 复杂度，利用双向图的 out_edges 接口
template <typename G>
std::vector<typename G::id_type> successors(const G &graph,
                                            typename G::id_type u) {
  using id_type = typename G::id_type;
  std::vector<id_type> succs;
  for (const auto &e : graph.out_edges(u)) {
    succs.push_back(e.to);
  }
  return succs;
}
// ----------------------------------------------------------
// 5. Transitive reduction on an edge list (node_count, edges).
//    Removes edge u->v if v is reachable from u via an alternate path.
// ----------------------------------------------------------
template <class T>
inline std::vector<std::pair<T, T>>
transitive_reduction(std::size_t node_count,
                     const std::vector<std::pair<T, T>> &edges) {
  if (edges.empty())
    return {};

  // 1. 构建邻接表 (Adjacency List)
  // 因为输入只是 Edge List，这一步是必须的，无法优化掉
  std::vector<std::vector<std::size_t>> adj(node_count);
  for (const auto &[u, v] : edges) {
    if (u < node_count && v < node_count)
      adj[u].push_back(v);
  }

  std::vector<std::pair<T, T>> pruned;
  pruned.reserve(edges.size());

  // ✨ 优化重点 1: 内存提升到循环外 (Allocation Hoisting)
  // 我们不再在循环里 vector<char> seen，而是用 int 做版本控制
  std::vector<size_t> seen(node_count, 0);
  size_t seen_version = 0;

  std::vector<std::size_t> stack;
  stack.reserve(node_count); // 预分配最大可能深度

  for (const auto &[u, v] : edges) {
    if (u >= node_count || v >= node_count)
      continue;

    // ✨ 优化重点 2: O(1) 的重置魔法 (Version Tagging)
    // 每次换一条边测试时，与其把 seen 全部填 0 (这是 O(N))，
    // 不如把版本号 +1。
    // 只有当 seen[node] == current_version 时，才算这次遍历访问过。
    seen_version++;

    // 防止极其罕见的溢出 (跑了 40 亿次循环)
    if (seen_version == 0) {
      std::fill(seen.begin(), seen.end(), 0);
      seen_version = 1;
    }

    stack.clear();

    // 寻找是否存在 u -> ... -> v 的**替代路径**
    // 也就是把 u 的所有邻居放入栈，但**排除**直接连向 v 的那个
    for (auto nxt : adj[u]) {
      if (nxt == v)
        continue;
      stack.push_back(nxt);
    }

    bool alternate_found = false;

    while (!stack.empty()) {
      auto curr = stack.back();
      stack.pop_back();

      // 找到了替代路径！说明直连边 (u->v) 是多余的
      if (curr == v) {
        alternate_found = true;
        break;
      }

      // ✨ 检查版本号代替检查 bool
      if (seen[curr] == seen_version)
        continue;

      seen[curr] = seen_version; // 标记为当前版本已访问

      for (auto nxt : adj[curr]) {
        stack.push_back(nxt);
      }
    }

    // 如果没有找到替代路径，说明这条边是必不可少的
    if (!alternate_found) {
      pruned.emplace_back(u, v);
    }
  }

  return pruned;
}

} // namespace graph::algo
