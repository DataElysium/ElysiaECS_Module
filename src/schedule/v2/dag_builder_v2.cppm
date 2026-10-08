module;
#include <vector>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>
#include <functional>

export module elysia.schedule:dag_builder;
import :graph_types;
import elysia.schedule.components;
import elysia.world;
import elysia.entity;
import graph;
import graph.algo;

export namespace elysia::schedule {
 
    using ScheduleGraph = graph::DirectedGraph<GraphNode>;

    /**
     * @brief Translates Scheduler meta-world into a logical DAG with SetStart/End sentinels.
     */
    inline ScheduleGraph build_dag(World& meta) {
        ScheduleGraph dag;

        auto endpoint = [&](Entity e, bool start) {
            auto type = meta.get_component<SystemTag>(e) ? GraphNode::System :
                meta.get_component<SetTag>(e) ? (start ? GraphNode::SetStart : GraphNode::SetEnd) : GraphNode::Unresolved;
            return GraphNode{type, e};
        };

        // 1. Pass: Create Nodes
        auto q_nodes = meta.query<Entity>();
        q_nodes.each([&](Entity e) {
            auto view = meta.entity(e);
            if (view.get<SystemTag>()) {
                dag.add_node({GraphNode::System, e});
            } else if (view.get<SetTag>()) {
                dag.add_node({GraphNode::SetStart, e});
                dag.add_node({GraphNode::SetEnd, e});
                dag.add_edge({GraphNode::SetStart, e}, {GraphNode::SetEnd, e});
            } else if (view.get<SysName>()) {
                dag.add_node({GraphNode::Unresolved, e});
            }
        });

        // 2. Pass: Resolve Hierarchy (InSet)
        auto q_inset = meta.query<Entity, InSet>();
        q_inset.each([&](Entity child, InSet& inset) {
            GraphNode p_start{GraphNode::SetStart, inset.target};
            GraphNode p_end{GraphNode::SetEnd, inset.target};

            if (meta.get_component<SystemTag>(child)) {
                GraphNode s_node{GraphNode::System, child};
                dag.add_edge(p_start, s_node);
                dag.add_edge(s_node, p_end);
            } else if (meta.get_component<SetTag>(child)) {
                GraphNode c_start{GraphNode::SetStart, child};
                GraphNode c_end{GraphNode::SetEnd, child};
                dag.add_edge(p_start, c_start);
                dag.add_edge(c_end, p_end);
            }
        });

        // 3. Pass: Resolve Dependencies (DependsOn)
        auto q_dep = meta.query<Entity, DependsOn>();
        q_dep.each([&](Entity src, DependsOn& dep) {
            for (auto target : dep.targets) {
                auto src_entry = endpoint(src, true);
                auto tgt_exit = endpoint(target, false);

                if (dag.has_node(src_entry) && dag.has_node(tgt_exit)) {
                    dag.add_edge(tgt_exit, src_entry);
                }
            }
        });

        return dag;
    }

    enum class MissingLabelPolicy { Fill, Reject };
    struct CompileOptions {
        MissingLabelPolicy missing_labels = MissingLabelPolicy::Fill;
        bool prune_empty_endpoints = true;
    };

    // Compile a separate graph. Neither metadata nor the logical graph is mutated.
    inline ScheduleGraph compile_dag(World& meta, CompileOptions options = {}) {
        auto logical = build_dag(meta);
        if (options.missing_labels == MissingLabelPolicy::Reject) {
            std::string missing;
            for (size_t i = 0; i < logical.node_count(); ++i) {
                const auto& node = logical.key(i);
                if (node.type != GraphNode::Unresolved) continue;
                auto* name = meta.get_component<SysName>(node.entity);
                if (!missing.empty()) missing += ", ";
                missing += name ? name->value : std::to_string(node.entity.id());
                std::string connected;
                for (size_t j = 0; j < logical.node_count(); ++j) {
                    if (j == i) continue;
                    bool linked = false;
                    for (const auto& edge : logical.out_edges(j)) if (edge.to == i) linked = true;
                    for (const auto& edge : logical.out_edges(i)) if (edge.to == j) linked = true;
                    if (linked) {
                        auto* neighbor = meta.get_component<SysName>(logical.key(j).entity);
                        if (!connected.empty()) connected += ", ";
                        connected += neighbor ? neighbor->value : std::to_string(j);
                    }
                }
                if (!connected.empty()) missing += " (connected to: " + connected + ")";
            }
            if (!missing.empty()) throw std::logic_error("Unresolved schedule labels: " + missing);
        }
        if (graph::algo::kahn_layers(logical).has_cycle) {
            std::vector<int> state(logical.node_count());
            std::vector<size_t> path;
            std::string cycle;
            std::function<bool(size_t)> visit = [&](size_t i) {
                state[i] = 1; path.push_back(i);
                for (const auto& edge : logical.out_edges(i)) {
                    if (state[edge.to] == 0 && visit(edge.to)) return true;
                    if (state[edge.to] == 1) {
                        bool recording = false;
                        for (auto node : path) {
                            if (node == edge.to) recording = true;
                            if (!recording) continue;
                            auto* name = meta.get_component<SysName>(logical.key(node).entity);
                            if (!cycle.empty()) cycle += " -> ";
                            cycle += name ? name->value : std::to_string(node);
                        }
                        auto* name = meta.get_component<SysName>(logical.key(edge.to).entity);
                        cycle += " -> " + (name ? name->value : std::to_string(edge.to));
                        return true;
                    }
                }
                path.pop_back(); state[i] = 2; return false;
            };
            for (size_t i = 0; i < logical.node_count(); ++i) if (!state[i] && visit(i)) break;
            throw std::logic_error("Schedule dependency cycle: " + cycle);
        }
        if (!options.prune_empty_endpoints) return logical;

        const auto count = logical.node_count();
        std::vector<size_t> incoming(count), outgoing(count), pending;
        std::vector<std::vector<size_t>> predecessors(count);
        std::vector<bool> removed(count);
        for (size_t i = 0; i < count; ++i)
            for (const auto& edge : logical.out_edges(i)) {
                ++outgoing[i]; ++incoming[edge.to];
                predecessors[edge.to].push_back(i);
            }
        auto enqueue = [&](size_t i) {
            // Registered callables, including empty lambdas, are never inferred to be no-ops.
            if (logical.key(i).type != GraphNode::System && !removed[i] &&
                (incoming[i] == 0 || outgoing[i] == 0)) {
                removed[i] = true;
                pending.push_back(i);
            }
        };
        for (size_t i = 0; i < count; ++i) enqueue(i);
        for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
            auto i = pending[cursor];
            for (const auto& edge : logical.out_edges(i)) { --incoming[edge.to]; enqueue(edge.to); }
            for (auto pred : predecessors[i]) { --outgoing[pred]; enqueue(pred); }
        }
        ScheduleGraph plan;
        for (size_t i = 0; i < count; ++i) if (!removed[i]) plan.add_node(logical.key(i));
        for (size_t i = 0; i < count; ++i) if (!removed[i])
            for (const auto& edge : logical.out_edges(i)) if (!removed[edge.to])
                plan.add_edge(logical.key(i), logical.key(edge.to));
        return plan;
    }

    enum class MermaidDirection { TopDown, LeftToRight };
    enum class MermaidLayout { GroupedSets, Flat, CompactSets };

    // Export the declared DAG, not executor-specific edges or a timing trace.
    // Numeric node IDs keep user-provided names out of Mermaid syntax.
    inline std::string to_mermaid(const ScheduleGraph& dag, World& meta,
                                  MermaidDirection direction = MermaidDirection::TopDown,
                                  MermaidLayout layout = MermaidLayout::GroupedSets) {
        auto escape = [](std::string_view name) {
            std::string result;
            for (unsigned char c : name) {
                if (c == '\n' || c == '\r' || c == '\t') result += ' ';
                else if (c == '"' || c == '&' || c == '<' || c == '>' || c == '#' ||
                         c == '`' || c == '\\' || c < 32 || c == 127)
                    result += "#" + std::to_string(c) + ";";
                else result += static_cast<char>(c);
            }
            return result;
        };
        std::string result = direction == MermaidDirection::LeftToRight ? "flowchart LR\n" : "flowchart TD\n";
        // Compact boxes already express membership. Hide only containment
        // wiring and unused boundaries; every non-containment edge stays visible.
        auto containment = [&](size_t from, size_t to) {
            const auto& a = dag.key(from);
            const auto& b = dag.key(to);
            if (a.type == GraphNode::SetStart && b.type == GraphNode::SetEnd && a.entity == b.entity)
                return true;
            const auto* child = meta.get_component<InSet>(b.entity);
            if (a.type == GraphNode::SetStart && child && child->target == a.entity &&
                b.type != GraphNode::SetEnd) return true;
            child = meta.get_component<InSet>(a.entity);
            return b.type == GraphNode::SetEnd && child && child->target == b.entity &&
                   a.type != GraphNode::SetStart;
        };
        std::vector<std::vector<size_t>> arrows(dag.node_count());
        std::vector<bool> visible(dag.node_count());
        for (size_t i = 0; i < dag.node_count(); ++i) {
            visible[i] = layout != MermaidLayout::CompactSets ||
                         (dag.key(i).type != GraphNode::SetStart && dag.key(i).type != GraphNode::SetEnd);
        }
        for (size_t i = 0; i < dag.node_count(); ++i) {
            for (const auto& edge : dag.out_edges(i))
                if (layout != MermaidLayout::CompactSets || !containment(i, edge.to)) {
                    arrows[i].push_back(edge.to);
                    visible[i] = visible[edge.to] = true;
                }
        }
        auto declaration = [&](size_t i) {
            const auto& node = dag.key(i);
            auto* name = meta.get_component<SysName>(node.entity);
            std::string label = name ? escape(name->value) : "Entity " + std::to_string(node.entity.id());
            std::string style;
            if (node.type == GraphNode::Unresolved) {
                label += " [unresolved]";
                style = "unresolved";
            } else if (node.type != GraphNode::System) {
                label += node.type == GraphNode::SetStart ? " [set start]" : " [set end]";
                style = "boundary";
            } else {
                if (auto* exec = meta.get_component<SysExecutor>(node.entity)) {
                    if (exec->kind == SpecialSystemKind::ApplyDeferred) { label += " [ApplyDeferred]"; style = "exclusive"; }
                    else if (exec->threading == ThreadingModel::Exclusive) { label += " [exclusive]"; style = "exclusive"; }
                    if (exec->affinity == ThreadAffinity::Caller) label += " [caller thread]";
                }
            }
            result += "  n" + std::to_string(i) + "[\"" + label + "\"]";
            if (!style.empty()) result += ":::" + style;
            result += '\n';
        };
        // Group only sets present in this graph. Compiled/pruned graphs need not
        // contain every declared parent. Node IDs and all DAG edges stay intact.
        std::unordered_map<uint64_t, size_t> groups;
        std::vector<size_t> representatives;
        if (layout != MermaidLayout::Flat)
            for (size_t i = 0; i < dag.node_count(); ++i) {
                const auto& node = dag.key(i);
                if (node.type == GraphNode::SetStart || node.type == GraphNode::SetEnd)
                    if (groups.emplace(node.entity.id(), representatives.size()).second)
                        representatives.push_back(i);
            }
        const size_t root = representatives.size();
        std::vector<std::vector<size_t>> members(root + 1), children(root + 1);
        auto parent = [&](Entity entity) {
            auto* membership = meta.get_component<InSet>(entity);
            if (membership) {
                auto it = groups.find(membership->target.id());
                if (it != groups.end()) return it->second;
            }
            return root;
        };
        for (size_t g = 0; g < root; ++g)
            children[parent(dag.key(representatives[g]).entity)].push_back(g);
        for (size_t i = 0; i < dag.node_count(); ++i) {
            const auto& node = dag.key(i);
            auto group = groups.find(node.entity.id());
            if (visible[i]) members[group != groups.end() ? group->second : parent(node.entity)].push_back(i);
        }
        std::vector<bool> emitted(root);
        auto emit = [&](auto&& self, size_t g) -> void {
            // Export remains diagnostic for cyclic set hierarchies too: each
            // group is emitted once, without recursing indefinitely.
            if (emitted[g]) return;
            emitted[g] = true;
            const auto& node = dag.key(representatives[g]);
            auto* name = meta.get_component<SysName>(node.entity);
            result += "  subgraph g" + std::to_string(representatives[g]) + "[\"" +
                      (name ? escape(name->value) : "System set") + "\"]\n";
            for (auto i : members[g]) declaration(i);
            for (auto child : children[g]) self(self, child);
            result += "  end\n";
        };
        for (auto i : members[root]) declaration(i);
        for (auto g : children[root]) emit(emit, g);
        for (size_t g = 0; g < root; ++g) emit(emit, g);
        for (size_t i = 0; i < dag.node_count(); ++i)
            for (auto target : arrows[i])
                result += "  n" + std::to_string(i) + " --> n" + std::to_string(target) + "\n";
        if (dag.node_count()) {
            result += "  classDef unresolved fill:#fff4dc,stroke:#9a6700,stroke-dasharray:5 5,color:#352600\n";
            result += "  classDef exclusive fill:#fce8e6,stroke:#a33,color:#511\n";
            result += "  classDef boundary fill:#e8eef5,stroke:#567,color:#234\n";
        }
        return result;
    }

    inline std::string to_mermaid(World& meta, MermaidDirection direction = MermaidDirection::TopDown,
                                  MermaidLayout layout = MermaidLayout::GroupedSets) {
        return to_mermaid(build_dag(meta), meta, direction, layout);
    }

} // namespace elysia::schedule

 