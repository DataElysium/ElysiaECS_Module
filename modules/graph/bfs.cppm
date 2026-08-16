module;

export module graph.algo.bfs; // 模块名字
import std;
import graph.traverse; 


export namespace  graph::algo {
using  graph::traverse::BFSRange;
template<class G>
class BFSTrait {
public:
    using id_type = std::size_t;

    explicit BFSTrait(const G& g) : g_(g) {}

    // 深度图：不可达 = -1
    std::vector<int> build_depth_map(id_type start) const {
        const auto n = g_.node_count();
        if (start >= n) throw std::out_of_range("start id out of range");

        std::vector<int> depth(n, -1);
        depth[start] = 0;

        // BFSRange 产出的是 id
        for (auto u : BFSRange<G>(g_, start)) {
            int du = depth[u];
            for (auto& e : g_.out_edges(u)) {
                auto v = e.to;
                if (depth[v] == -1) depth[v] = du + 1;
            }
        }
        return depth;
    }

    // 有向环检测（从 start 可达子图上）
    bool has_cycle_reachable(id_type start) const {
        const auto n = g_.node_count();
        if (start >= n) throw std::out_of_range("start id out of range");

        // 0=未见过, 1=正在栈上(灰), 2=完成(黑)
        std::vector<unsigned char> color(n, 0);

        // 显式栈： (node, next_edge_index)
        std::vector<std::pair<id_type,std::size_t>> st;
        st.reserve(n);

        auto push_node = [&](id_type u){
            color[u] = 1;
            st.emplace_back(u, 0);
        };

        push_node(start);

        while (!st.empty()) {
            auto& [u, idx] = st.back();
            auto& edges = g_.out_edges(u);

            if (idx == edges.size()) {
                color[u] = 2;
                st.pop_back();
                continue;
            }

            id_type v = edges[idx++].to;

            if (color[v] == 1) {
                return true; // 发现回边 => 有向环
            }
            if (color[v] == 0) {
                push_node(v);
            }
        }
        return false;
    }

    // 有向“根树/分支树(arborescence)”判定：
    // 1) 从 start 可达所有节点
    // 2) 可达子图无环
    // 3) 每个非 root 节点的可达入度恰好为 1
    bool is_arborescence(id_type start) const {
        const auto n = g_.node_count();
        if (start >= n) throw std::out_of_range("start id out of range");

        // 可达性 + 入度（只算可达子图）
        std::vector<unsigned char> reachable(n, 0);
        std::vector<int> indeg(n, 0);

        for (auto u : BFSRange<G>(g_, start)) {
            reachable[u] = 1;
            for (auto& e : g_.out_edges(u)) {
                id_type v = e.to;
                if (!reachable[v]) {
                    // 还没标可达前，BFS 也会 eventually 标，这里不提前
                }
                indeg[v] += 1;
            }
        }

        // 1) 全可达
        for (auto r : reachable) if (!r) return false;

        // 2) 无环
        if (has_cycle_reachable(start)) return false;

        // 3) 入度约束
        if (indeg[start] != 0) return false;
        for (id_type u = 0; u < n; ++u) {
            if (u == start) continue;
            if (indeg[u] != 1) return false;
        }
        return true;
    }

private:
    const G& g_;
};

} // namespace ecs::graph::traverse