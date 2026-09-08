#include <gtest/gtest.h>
#include <vector>
#include <stdexcept>
import graph;
import elysia;
import elysia.plugins.hierarchy;

namespace hierarchy_api_test {
using namespace elysia;

TEST(HierarchyAPI, AttachDetachAndReparentMaintainBothDirections) {
    World world;
    install_hierarchy(world);
    install_hierarchy(world);
    auto a = world.spawn().entity;
    auto b = world.spawn().entity;
    auto child = world.spawn().entity;
    attach_child(world, a, child);
    attach_child(world, a, child);
    world.entity(child).add(ChildOf{a}); // repeated same-parent insertion is idempotent
    ASSERT_EQ(world.get_component<Children>(a)->ids.size(), 1);
    reparent(world, child, b);
    EXPECT_TRUE(world.get_component<Children>(a)->ids.empty());
    EXPECT_EQ(world.get_component<Children>(b)->ids, (std::vector<Entity>{child}));
    EXPECT_EQ(world.get_component<ChildOf>(child)->parent, b);
    detach_child(world, child);
    EXPECT_EQ(world.get_component<ChildOf>(child), nullptr);
    EXPECT_TRUE(world.get_component<Children>(b)->ids.empty());
    detach_child(world, child);
}
TEST(HierarchyAPI, ReparentRejectsCyclesAndDeadTargetsBeforeChangingLinks) {
    World world;
    auto root = world.spawn().entity;
    auto child = world.spawn().entity;
    auto leaf = world.spawn().entity;
    attach_child(world, root, child);
    attach_child(world, child, leaf);
    EXPECT_THROW(reparent(world, root, leaf), std::invalid_argument);
    EXPECT_THROW(reparent(world, child, child), std::invalid_argument);
    auto dead = world.spawn().entity;
    world.despawn(dead);
    EXPECT_THROW(reparent(world, child, dead), std::invalid_argument);
    EXPECT_EQ(collect_subtree(world, root), (std::vector<Entity>{root, child, leaf}));
    EXPECT_EQ(world.get_component<ChildOf>(child)->parent, root);
}
TEST(HierarchyAPI, GraphAndDirectTraversalStayLocalAndReflectEdits) {
    World world;
    auto root = world.spawn().entity;
    auto a = world.spawn().entity;
    auto b = world.spawn().entity;
    attach_child(world, root, a);
    attach_child(world, root, b);
    auto unrelated = world.spawn().entity;
    for (int i = 0; i < 100; ++i) attach_child(world, unrelated, world.spawn().entity);
    auto graph = build_hierarchy_graph(world, root);
    EXPECT_EQ(graph.node_count(), 3);
    EXPECT_EQ(graph.out_degree(graph.id(root)), 2);
    EXPECT_FALSE(graph.has_node(unrelated));
    reparent(world, b, a);
    auto updated = build_hierarchy_graph(world, root);
    EXPECT_EQ(updated.out_degree(updated.id(root)), 1);
    EXPECT_EQ(updated.in_edges(updated.id(b)), (std::vector<graph::id_type>{updated.id(a)}));
    world.despawn(a);
    EXPECT_EQ(collect_subtree(world, root), (std::vector<Entity>{root}));
    EXPECT_EQ(build_hierarchy_graph(world, root).node_count(), 1);
    EXPECT_TRUE(world.index().is_alive(unrelated));
}
TEST(HierarchyAPI, PluginAndPlainWorldInstallationUseTheSameRelationships) {
    App app;
    app.add_plugin(HierarchyPlugin{});
    install_hierarchy(app.world());
    auto parent = app.world().spawn().entity;
    std::vector<Entity> descendants;
    for (int i = 0; i < 32; ++i) {
        auto child = app.world().spawn().add(ChildOf{parent}).entity;
        descendants.push_back(child);
        descendants.push_back(app.world().spawn().add(ChildOf{child}).entity);
    }
    EXPECT_EQ(collect_subtree(app.world(), parent).size(), 65);
    app.world().despawn(parent);
    EXPECT_FALSE(app.world().index().is_alive(parent));
    for (auto entity : descendants) EXPECT_FALSE(app.world().index().is_alive(entity));
}
TEST(HierarchyAPI, DeepCascadeUsesIterativeSubtreeDeletion) {
    World world;
    install_hierarchy(world);
    auto root = world.spawn().entity;
    auto parent = root;
    std::vector<Entity> descendants;
    for (int i = 0; i < 1500; ++i) {
        parent = world.spawn().add(ChildOf{parent}).entity;
        descendants.push_back(parent);
    }
    EXPECT_EQ(collect_subtree(world, root).size(), 1501);
    world.despawn(root);
    for (auto entity : descendants) EXPECT_FALSE(world.index().is_alive(entity));
}
} // namespace hierarchy_api_test
