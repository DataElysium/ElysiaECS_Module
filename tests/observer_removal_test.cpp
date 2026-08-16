#include <gtest/gtest.h>
#include <vector>

import elysia;

using namespace elysia;

struct TagR {};

TEST(ObserverTest, OnAdd) {
    App app;
    int add_count = 0;

    app.observer<OnAdd, TagR>().run([&](Entity e) {
        add_count++;
    });

    auto e = app.world().spawn();
    e.add(TagR{});
    EXPECT_EQ(add_count, 1);
}

TEST(ObserverTest, OnRemove) {
    App app;
    int remove_count = 0;

    app.observer<OnRemove, TagR>().run([&](Entity e) {
        remove_count++;
    });

    auto e = app.world().spawn();
    e.add(TagR{});
    EXPECT_EQ(remove_count, 0);

    e.remove<TagR>();
    EXPECT_EQ(remove_count, 1);
}

TEST(ObserverTest, OnDespawn) {
    App app;
    int remove_count = 0;

    app.observer<OnRemove, TagR>().run([&](Entity e) {
        remove_count++;
    });

    auto e = app.world().spawn();
    e.add(TagR{});
    
    e.despawn();
    EXPECT_EQ(remove_count, 1);
}

TEST(ObserverTest, BatchDespawnKeepsOnRemove) {
    App app;
    int remove_count = 0;

    app.observer<OnRemove, TagR>().run([&](Entity) {
        remove_count++;
    });

    auto e1 = app.world().spawn().add(TagR{}).entity;
    auto e2 = app.world().spawn().add(TagR{}).entity;

    std::vector<Entity> batch{e1, e2};
    app.world().batch_despawn(batch);

    EXPECT_EQ(remove_count, 2);
    EXPECT_FALSE(app.world().index().is_alive(e1));
    EXPECT_FALSE(app.world().index().is_alive(e2));
}

TEST(ObserverTest, DropArchetypesWithObserverKeepsOnRemove) {
    App app;
    int remove_count = 0;

    app.observer<OnRemove, TagR>().run([&](Entity) {
        remove_count++;
    });

    auto e1 = app.world().spawn().add(TagR{}).entity;
    auto e2 = app.world().spawn().add(TagR{}).entity;

    size_t removed = app.world().drop_archetypes_with<TagR>();

    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(remove_count, 2);
    EXPECT_FALSE(app.world().index().is_alive(e1));
    EXPECT_FALSE(app.world().index().is_alive(e2));
}
