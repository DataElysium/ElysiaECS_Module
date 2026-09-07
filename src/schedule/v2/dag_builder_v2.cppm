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
            }
            if (!missing.empty()) throw std::logic_error("Unresolved schedule labels: " + missing);
        }
        if (graph::algo::kahn_layers(logical).has_cycle)
            throw std::logic_error("Schedule contains a dependency cycle");
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

    // Export the declared DAG, not executor-specific edges or a timing trace.
    // Numeric node IDs keep user-provided names out of Mermaid syntax.
    inline std::string to_mermaid(const ScheduleGraph& dag, World& meta,
                                  MermaidDirection direction = MermaidDirection::TopDown) {
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
        for (size_t i = 0; i < dag.node_count(); ++i) {
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
                }
            }
            result += "  n" + std::to_string(i) + "[\"" + label + "\"]";
            if (!style.empty()) result += ":::" + style;
            result += '\n';
        }
        for (size_t i = 0; i < dag.node_count(); ++i)
            for (const auto& edge : dag.out_edges(i))
                result += "  n" + std::to_string(i) + " --> n" + std::to_string(edge.to) + "\n";
        if (dag.node_count()) {
            result += "  classDef unresolved fill:#fff4dc,stroke:#9a6700,stroke-dasharray:5 5,color:#352600\n";
            result += "  classDef exclusive fill:#fce8e6,stroke:#a33,color:#511\n";
            result += "  classDef boundary fill:#e8eef5,stroke:#567,color:#234\n";
        }
        return result;
    }

    inline std::string to_mermaid(World& meta, MermaidDirection direction = MermaidDirection::TopDown) {
        return to_mermaid(build_dag(meta), meta, direction);
    }

} // namespace elysia::schedule

 