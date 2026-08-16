module;
#include <gtest/gtest.h>
#include <algorithm>

export module elysia_hierarchy_test;

import elysia;
import elysia.app;
import elysia.plugins.hierarchy;

using namespace elysia;

TEST(ElysiaHierarchy, CascadeDelete) {
    App app;
    app.add_plugin(HierarchyPlugin{});

    // 1. Create Parent
    auto parent = app.world().spawn().entity;
    
    // 2. Create Children
    auto child1 = app.world().spawn().add(ChildOf{parent}).entity;
    auto child2 = app.world().spawn().add(ChildOf{parent}).entity;

    // Flush observers (OnAdd ChildOf)
    // Note: App::update() runs scheduler. Observers run immediately on trigger?
    // No, Observers run when triggered. 
    // spawn().add() triggers OnAdd immediately in Elysia?
    // let's check add_component_dynamic in world_impl.cpp.
    // Yes: world.observer().notify(...) is called inside OpCode::Insert processing or add_component_dynamic.
    // But spawn().add() uses CommandBuffer usually? No, EntityView::add calls world->add_component_dynamic directly?
    // Let's check EntityView::add in world.cppm.
    // "world->add_component_dynamic(entity, ...)" -> Yes, immediate!
    
    // So relationships should be established now.
    auto* children = app.world().get_component<Children>(parent);
    ASSERT_NE(children, nullptr);
    EXPECT_EQ(children->ids.size(), 2);
    EXPECT_EQ(children->ids[0], child1);
    EXPECT_EQ(children->ids[1], child2);

    // 3. Kill Parent
    app.world().despawn(parent);
    
    // Despawn is immediate in EntityView::despawn? 
    // EntityView::despawn -> world->despawn(e).
    // world->despawn calls index_.free(e) and removes from archetype.
    // It SHOULD trigger OnRemove now (after my fix).
    // Inside OnRemove<Children>, it calls app.world().despawn(child).
    // So this is recursive/nested immediate call.
    
    // 4. Verify Children are dead
    EXPECT_FALSE(app.world().index().is_alive(parent));
    EXPECT_FALSE(app.world().index().is_alive(child1));
    EXPECT_FALSE(app.world().index().is_alive(child2));
}

TEST(ElysiaHierarchy, UnlinkOnChildDeath) {
    App app;
    app.add_plugin(HierarchyPlugin{});

    auto parent = app.world().spawn().entity;
    auto child = app.world().spawn().add(ChildOf{parent}).entity;

    // Verify link
    auto* children = app.world().get_component<Children>(parent);
    ASSERT_NE(children, nullptr);
    EXPECT_EQ(children->ids.size(), 1);

    // Kill Child
    app.world().despawn(child);

    // Verify parent forgot child
    // Children component should still exist (parent is alive)
    children = app.world().get_component<Children>(parent);
    ASSERT_NE(children, nullptr);
    EXPECT_TRUE(children->ids.empty());
}
