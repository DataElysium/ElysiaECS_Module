module;

export module graph.traverse; // 模块名字
import std;
export namespace graph::traverse {

// 一个小 concept：只要求图有 node_count() 和 out_edges(id)
template <class G>
concept IdOutGraph = requires(const G &g, std::size_t id) {
  { g.node_count() } -> std::convertible_to<std::size_t>;
  { g.out_edges(id) }; // range of edges
  // edge has member .to
  { g.out_edges(id).begin()->to } -> std::convertible_to<std::size_t>;
};

// =============================
// BFS Range
// =============================
template <IdOutGraph G> class BFSRange {
public:
  using id_type = std::size_t;

  BFSRange(const G &g, id_type start) : g_(g), visited_(g.node_count(), 0) {
    if (start >= g_.node_count())
      throw std::out_of_range("BFS start id out of range");
    visited_[start] = 1;
    q_.push(start);
  }

  struct iterator {
    const G *g{};
    std::vector<unsigned char> *visited{};
    std::queue<id_type> *q{};

    id_type operator*() const { return q->front(); }

    iterator &operator++() {
      id_type u = q->front();
      q->pop();

      for (auto &e : g->out_edges(u)) {
        id_type v = e.to;
        if (!(*visited)[v]) {
          (*visited)[v] = 1;
          q->push(v);
        }
      }
      return *this;
    }

    bool operator==(std::default_sentinel_t) const { return q->empty(); }
  };

  iterator begin() { return iterator{&g_, &visited_, &q_}; }
  std::default_sentinel_t end() const { return {}; }

private:
  const G &g_;
  std::vector<unsigned char> visited_;
  std::queue<id_type> q_;
};

// =============================
// DFS Range
// （非递归，前序；同样无 hash）
// =============================
template <IdOutGraph G> class DFSRange {
public:
  using id_type = std::size_t;

  DFSRange(const G &g, id_type start) : g_(g), visited_(g.node_count(), 0) {
    if (start >= g_.node_count())
      throw std::out_of_range("DFS start id out of range");
    st_.push(start);
  }

  struct iterator {
    const G *g{};
    std::vector<unsigned char> *visited{};
    std::stack<id_type> *st{};
    id_type current{};

    id_type operator*() const { return current; }

    iterator &operator++() {
      advance();
      return *this;
    }

    bool operator==(std::default_sentinel_t) const {
      return st->empty() && current == invalid();
    }

    static constexpr id_type invalid() { return static_cast<id_type>(-1); }

    void advance() {
      current = invalid();
      while (!st->empty()) {
        id_type u = st->top();
        st->pop();
        if ((*visited)[u])
          continue;
        (*visited)[u] = 1;
        current = u;

        // 为了让 DFS 顺序更直观，逆序压栈
        auto &edges = g->out_edges(u);
        for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
          id_type v = it->to;
          if (!(*visited)[v])
            st->push(v);
        }
        break;
      }
    }
  };

  iterator begin() {
    iterator it{&g_, &visited_, &st_};
    it.advance(); // 先定位第一个
    return it;
  }
  std::default_sentinel_t end() const { return {}; }

private:
  const G &g_;
  std::vector<unsigned char> visited_;
  std::stack<id_type> st_;
};

} // namespace graph::traverse
