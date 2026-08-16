module;
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <functional>

export module elysia.schedule:dag_builder;
import :graph_types;
import elysia.schedule.components;
import elysia.world;
import elysia.entity;
import graph;

export namespace elysia::schedule {
 
    using ScheduleGraph = graph::DirectedGraph<GraphNode>;

    /**
     * @brief Translates Scheduler meta-world into a physical DAG with SetStart/End sentinels.
     */
    inline ScheduleGraph build_dag(World& meta) {
        ScheduleGraph dag;

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
                GraphNode src_entry = meta.get_component<SystemTag>(src) ? 
                                      GraphNode{GraphNode::System, src} : GraphNode{GraphNode::SetStart, src};
                
                GraphNode tgt_exit = meta.get_component<SystemTag>(target) ? 
                                     GraphNode{GraphNode::System, target} : GraphNode{GraphNode::SetEnd, target};

                if (dag.has_node(src_entry) && dag.has_node(tgt_exit)) {
                    dag.add_edge(tgt_exit, src_entry);
                }
            }
        });

        return dag;
    }

} // namespace elysia::schedule

 