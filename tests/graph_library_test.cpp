#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

import graph;
import graph.algo;
import graph.traverse;
import graph.algo.bfs;

namespace graph_library_test {
using Graph = graph::DirectedGraph<int>;

// Compare both adjacency directions by public node keys, including parallel edges.
template <class G> void expect_consistent(const G &g) {
    std::multiset<std::pair<int, int>> outgoing, incoming;
    for (graph::id_type u = 0; u < g.node_count(); ++u) {
        EXPECT_EQ(g.id(g.key(u)), u);
        for (const auto &edge : g.out_edges(u)) {
            ASSERT_TRUE(g.is_valid(edge.to));
            outgoing.emplace(g.key(u), g.key(edge.to));
        }
        for (auto source : g.in_edges(u)) {
            ASSERT_TRUE(g.is_valid(source));
            incoming.emplace(g.key(source), g.key(u));
        }
    }
    EXPECT_EQ(outgoing, incoming);
}

TEST(GraphLibrary, WeightedEdgesRespectDependencyOrder) {
    graph::DirectedGraph<int, void, double> g;
    g.add_node(0);
    g.add_node(1);
    g.add_edge(1, 0, 2.5);
    expect_consistent(g);
    EXPECT_EQ(g.in_degree(g.id(0)), 1);
    auto order = graph::algo::kahn_layers(g);
    EXPECT_FALSE(order.has_cycle);
    EXPECT_EQ(order.nodes_sorted, (std::vector<std::size_t>{1, 0}));
    EXPECT_DOUBLE_EQ(g.out_edges(1).at(0).weight(), 2.5);
}
TEST(GraphLibrary, RemoveNodeRepairsBothDirections) {
    Graph g;
    g.add_edge(0, 1);
    ASSERT_TRUE(g.remove_node(0));
    expect_consistent(g);
    EXPECT_EQ(g.in_degree(g.id(1)), 0);
    EXPECT_FALSE(graph::algo::kahn_layers(g).has_cycle);
}
TEST(GraphLibrary, RemoveLastAndRelocatedNodesWithParallelEdgesAndSelfLoops) {
    Graph g;
    for (int i = 0; i < 4; ++i)
        g.add_node(i);
    g.add_edge(0, 1);
    g.add_edge(0, 1);
    g.add_edge(3, 2);
    g.add_edge(3, 3);
    g.add_edge(2, 3);
    ASSERT_TRUE(g.remove_node(1)); // node 3 moves into its slot
    expect_consistent(g);
    ASSERT_TRUE(g.remove_node(2)); // remove the last slot
    expect_consistent(g);
    EXPECT_TRUE(g.remove_edge(3, 3));
    expect_consistent(g);
    EXPECT_FALSE(graph::algo::kahn_layers(g).has_cycle);
}
TEST(GraphLibrary, TarjanRecognizesSelfLoopsAndDisconnectedCycles) {
    Graph g;
    g.add_edge(0, 0);
    g.add_node(1);
    EXPECT_TRUE(graph::algo::tarjan_scc(g).has_cycle);
    EXPECT_TRUE(graph::algo::kahn_layers(g).has_cycle);
    g.remove_edge(0, 0);
    EXPECT_FALSE(graph::algo::tarjan_scc(g).has_cycle);
    g.add_edge(2, 3);
    g.add_edge(3, 2);
    auto scc = graph::algo::tarjan_scc(g);
    EXPECT_TRUE(scc.has_cycle);
    EXPECT_EQ(scc.component_ids[g.id(2)], scc.component_ids[g.id(3)]);
    EXPECT_NE(scc.component_ids[g.id(0)], scc.component_ids[g.id(1)]);
}

TEST(GraphLibrary, RetainNodesPreservesInducedEdgesAndWeights) {
    graph::DirectedGraph<int, void, int> g;
    g.reserve_nodes(6);
    for (int i = 0; i < 6; ++i)
        g.add_node(i);
    g.add_edge(0, 1, 1);
    g.add_edge(0, 4, 4);
    g.add_edge(4, 2, 42);
    g.add_edge(4, 2, 43);
    g.add_edge(4, 4, 44);
    g.add_edge(2, 5, 25);
    g.retain_nodes([](int key) { return key % 2 == 0; });
    expect_consistent(g);
    EXPECT_EQ(g.node_count(), 3);
    EXPECT_EQ(g.key(0), 0);
    EXPECT_EQ(g.key(1), 2);
    EXPECT_EQ(g.key(2), 4);
    EXPECT_EQ(g.out_edges(g.id(4)).size(), 3);
    EXPECT_EQ(g.out_edges(g.id(4)).at(1).weight(), 43);
    EXPECT_TRUE(g.remove_edge(4, 2)); // removes both parallel edges
    expect_consistent(g);
    EXPECT_EQ(g.in_degree(g.id(2)), 0);
    g.retain_nodes([](int) { return true; });
    expect_consistent(g);
    g.retain_nodes([](int) { return false; });
    EXPECT_EQ(g.node_count(), 0);
    g.add_edge(9, 10, 1);
    expect_consistent(g);
}
TEST(GraphLibrary, ThrowingRetainPredicateLeavesGraphIntact) {
    Graph g;
    g.add_edge(0, 1);
    g.add_edge(1, 2);
    EXPECT_THROW(g.retain_nodes([](int key) {
        if (key == 2)
            throw std::runtime_error("predicate failed");
        return key != 0;
    }),
                 std::runtime_error);
    EXPECT_EQ(g.node_count(), 3);
    EXPECT_EQ(g.id(0), 0);
    expect_consistent(g);
    EXPECT_EQ(g.out_degree(g.id(0)), 1);
}

template <class W> void expect_sparse_exports() {
    graph::DirectedGraph<int, void, W> g;
    for (int i = 0; i < 6; ++i)
        g.add_node(i);
    for (const auto &[u, v, w] :
         std::vector<std::tuple<int, int, int>>{{0, 3, 3}, {0, 1, 1}, {2, 0, 20}, {3, 4, 34}, {3, 1, 31}}) {
        if constexpr (graph::Weighted<W>)
            g.add_edge(u, v, w);
        else
            g.add_edge(u, v);
    }
    auto csr = g.build_csr();
    auto csc = g.build_csc();
    EXPECT_EQ(csr.rows(), 6);
    EXPECT_EQ(csr.cols(), 6);
    EXPECT_EQ(csc.rows(), 6);
    EXPECT_EQ(csc.cols(), 6);
    EXPECT_EQ(csr.indptr, (std::vector<graph::id_type>{0, 2, 2, 3, 5, 5, 5}));
    EXPECT_EQ(csr.indices, (std::vector<graph::id_type>{1, 3, 0, 1, 4}));
    EXPECT_EQ(csc.indptr, (std::vector<graph::id_type>{0, 1, 3, 3, 4, 5, 5}));
    EXPECT_EQ(csc.indices, (std::vector<graph::id_type>{2, 0, 3, 0, 3}));
    EXPECT_EQ(g.build_csr(false).indices, (std::vector<graph::id_type>{3, 1, 0, 4, 1}));
    EXPECT_EQ(g.build_csc(false).indices, csc.indices);
    EXPECT_TRUE(csr.row_indices(5).empty());
    EXPECT_TRUE(csc.col_indices(5).empty());
    EXPECT_EQ(csr.row_indices(3).size(), 2);
    EXPECT_EQ(csc.col_indices(4).size(), 1);
    EXPECT_THROW(csr.row_indices(6), std::out_of_range);
    EXPECT_THROW(csr.row_weights(6), std::out_of_range);
    EXPECT_THROW(csc.col_indices(6), std::out_of_range);
    if constexpr (graph::Weighted<W>) {
        EXPECT_EQ(csr.data, (std::vector<W>{1, 3, 20, 31, 34}));
        EXPECT_EQ(csc.data, (std::vector<W>{20, 1, 31, 3, 34}));
        EXPECT_EQ(g.build_csr(false).data, (std::vector<W>{3, 1, 20, 34, 31}));
        EXPECT_TRUE(csr.row_weights(5).empty());
        EXPECT_EQ(csr.row_weights(3)[1], 34);
    }
    g.retain_nodes([](int) { return false; });
    EXPECT_EQ(g.build_csr().indptr, (std::vector<graph::id_type>{0}));
    EXPECT_EQ(g.build_csc().indptr, (std::vector<graph::id_type>{0}));
    EXPECT_EQ(g.build_csr().cols(), 0);
    g.add_node(7);
    EXPECT_TRUE(g.build_csr().row_indices(0).empty());
    EXPECT_TRUE(g.build_csc().col_indices(0).empty());
    EXPECT_TRUE(g.build_csr().row_weights(0).empty());
    g.add_edge(7, 7);
    EXPECT_EQ(g.build_csr().nnz(), 1);
    EXPECT_EQ(g.build_csc().nnz(), 1);
}
TEST(GraphLibrary, WeightedSparseExportsKeepOffsetsDimensionsAndWeights) {
    expect_sparse_exports<int>();
}
TEST(GraphLibrary, UnweightedSparseExportsAndEmptySlices) {
    expect_sparse_exports<void>();
}

// Independent key-based model: exercise mutation histories, not just one graph shape.
TEST(GraphLibrary, SeededMutationsMatchEdgeListModel) {
    graph::DirectedGraph<int, void, int> g;
    std::set<int> nodes;
    std::multiset<std::tuple<int, int, int>> edges;
    std::mt19937 random(0xE1751A);
    for (int step = 0; step < 400; ++step) {
        SCOPED_TRACE(step);
        int u = random() % 12, v = random() % 12, op = random() % 5;
        if (op == 0) {
            g.add_node(u);
            nodes.insert(u);
        }
        if (op == 1) {
            g.add_edge(u, v, step);
            nodes.insert(u);
            nodes.insert(v);
            edges.emplace(u, v, step);
        }
        if (op == 2) {
            auto count =
                std::erase_if(edges, [&](auto e) { return std::get<0>(e) == u && std::get<1>(e) == v; });
            EXPECT_EQ(g.remove_edge(u, v), count != 0);
        }
        if (op == 3) {
            EXPECT_EQ(g.remove_node(u), nodes.erase(u) != 0);
            std::erase_if(edges, [&](auto e) { return std::get<0>(e) == u || std::get<1>(e) == u; });
        }
        if (op == 4) {
            g.retain_nodes([&](int key) { return key % 3 != u % 3; });
            std::erase_if(nodes, [&](int key) { return key % 3 == u % 3; });
            std::erase_if(edges, [&](auto e) {
                return !nodes.contains(std::get<0>(e)) || !nodes.contains(std::get<1>(e));
            });
        }
        ASSERT_EQ(g.node_count(), nodes.size());
        expect_consistent(g);
        std::multiset<std::tuple<int, int, int>> actual, rows, columns;
        auto csr = g.build_csr();
        auto csc = g.build_csc();
        for (graph::id_type i = 0; i < g.node_count(); ++i) {
            EXPECT_TRUE(nodes.contains(g.key(i)));
            for (const auto &e : g.out_edges(i))
                actual.emplace(g.key(i), g.key(e.to), e.weight());
            for (auto k = csr.indptr[i]; k < csr.indptr[i + 1]; ++k)
                rows.emplace(g.key(i), g.key(csr.indices[k]), csr.data[k]);
            for (auto k = csc.indptr[i]; k < csc.indptr[i + 1]; ++k)
                columns.emplace(g.key(csc.indices[k]), g.key(i), csc.data[k]);
        }
        EXPECT_EQ(actual, edges);
        EXPECT_EQ(rows, edges);
        EXPECT_EQ(columns, edges);
    }
}
TEST(GraphLibrary, ExhaustiveThreeNodeGraphsAgreeWithTransitiveClosure) {
    for (unsigned mask = 0; mask < 512; ++mask) {
        SCOPED_TRACE(mask);
        Graph g;
        for (int i = 0; i < 3; ++i)
            g.add_node(i);
        bool reach[3][3]{};
        for (int u = 0; u < 3; ++u)
            for (int v = 0; v < 3; ++v)
                if (mask & (1u << (u * 3 + v))) {
                    g.add_edge(u, v);
                    reach[u][v] = true;
                }
        for (int k = 0; k < 3; ++k)
            for (int u = 0; u < 3; ++u)
                for (int v = 0; v < 3; ++v)
                    reach[u][v] = reach[u][v] || (reach[u][k] && reach[k][v]);
        bool cyclic = reach[0][0] || reach[1][1] || reach[2][2];
        auto scc = graph::algo::tarjan_scc(g);
        auto order = graph::algo::kahn_layers(g);
        EXPECT_EQ(scc.has_cycle, cyclic);
        EXPECT_EQ(order.has_cycle, cyclic);
        for (int u = 0; u < 3; ++u)
            for (int v = 0; v < 3; ++v)
                EXPECT_EQ(scc.component_ids[u] == scc.component_ids[v],
                          u == v || (reach[u][v] && reach[v][u]));
        if (!cyclic) {
            ASSERT_EQ(order.nodes_sorted.size(), 3);
            std::vector<int> layer(3, -1);
            int rank = 0;
            for (auto batch : order) {
                for (auto node : batch)
                    layer[node] = rank;
                ++rank;
            }
            for (int u = 0; u < 3; ++u)
                for (const auto &edge : g.out_edges(u))
                    EXPECT_LT(layer[u], layer[edge.to]);
        }
    }
}
TEST(GraphLibrary, KahnHandlesEmptyDiamondAndLongChain) {
    Graph g;
    auto empty = graph::algo::kahn_layers(g);
    EXPECT_EQ(empty.begin(), empty.end());
    graph::algo::KahnResult default_result;
    EXPECT_EQ(default_result.begin(), default_result.end());
    g.add_edge(0, 1);
    g.add_edge(0, 2);
    g.add_edge(1, 3);
    g.add_edge(2, 3);
    auto order = graph::algo::kahn_layers(g);
    EXPECT_EQ(order.layer_offsets, (std::vector<std::size_t>{0, 1, 3, 4}));
    auto other = graph::algo::kahn_layers(g);
    EXPECT_NE(order.begin(), other.begin());
    Graph chain;
    for (int i = 0; i < 10000; ++i)
        chain.add_edge(i, i + 1);
    auto layers = graph::algo::kahn_layers(chain);
    ASSERT_EQ(layers.nodes_sorted.size(), 10001);
    EXPECT_EQ(layers.layer_offsets.size(), 10002);
    for (std::size_t i = 0; i < layers.nodes_sorted.size(); ++i)
        EXPECT_EQ(layers.nodes_sorted[i], i);
}
TEST(GraphLibrary, TraversalAndTransparentNodesVisitEachReachableNodeOnce) {
    Graph g;
    g.add_edge(0, 1);
    g.add_edge(0, 2);
    g.add_edge(1, 3);
    g.add_edge(2, 3);
    g.add_edge(3, 1);
    g.add_node(4);
    std::vector<std::size_t> bfs, dfs;
    for (auto u : graph::traverse::BFSRange(g, 0))
        bfs.push_back(u);
    for (auto u : graph::traverse::DFSRange(g, 0))
        dfs.push_back(u);
    EXPECT_EQ(bfs, (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(dfs, (std::vector<std::size_t>{0, 1, 3, 2}));
    graph::algo::BFSTrait trait(g);
    EXPECT_EQ(trait.build_depth_map(0), (std::vector<int>{0, 1, 1, 2, -1}));
    EXPECT_TRUE(trait.has_cycle_reachable(0));
    EXPECT_FALSE(trait.is_arborescence(0));
    std::vector<std::size_t> reached;
    graph::algo::reach_through_transparent(
        g, 0, [](auto u) { return u == 1 || u == 2; }, [&](auto u) { reached.push_back(u); });
    EXPECT_EQ(reached, (std::vector<std::size_t>{3}));
    EXPECT_THROW(graph::traverse::BFSRange(g, 9), std::out_of_range);
    EXPECT_THROW(graph::traverse::DFSRange(g, 9), std::out_of_range);
    EXPECT_THROW(trait.build_depth_map(9), std::out_of_range);
    EXPECT_EQ(graph::algo::predecessors(g, 3).size(), 2);
    EXPECT_EQ(graph::algo::successors(g, 0).size(), 2);
}
TEST(GraphLibrary, TransitiveReductionPreservesDagDependencies) {
    std::vector<std::pair<int, int>> edges{{0, 1}, {1, 2}, {0, 2}, {2, 3}, {0, 3}};
    EXPECT_EQ(graph::algo::transitive_reduction(4, edges),
              (std::vector<std::pair<int, int>>{{0, 1}, {1, 2}, {2, 3}}));
}

} // namespace graph_library_test
