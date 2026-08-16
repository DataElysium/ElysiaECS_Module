#include <gtest/gtest.h>
#include <vector>

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

using namespace elysia;

struct Value { int v; };

TEST(ElysiaQueryEntity, EntityAsFirstArg) {
    World world;
    auto e1 = world.spawn().add(Value{10}).entity;
    auto e2 = world.spawn().add(Value{20}).entity;

    int count = 0;
    world.query<Entity, Value>().each([&](Entity e, Value& v) {
        if (e == e1) EXPECT_EQ(v.v, 10);
        if (e == e2) EXPECT_EQ(v.v, 20);
        count++;
    });
    EXPECT_EQ(count, 2);
}

TEST(ElysiaQueryEntity, EntityAsLastArg) {
    World world;
    auto e1 = world.spawn().add(Value{100}).entity;

    int count = 0;
    world.query<const Value, Entity>().each([&](const Value& v, Entity e) {
        EXPECT_EQ(e, e1);
        EXPECT_EQ(v.v, 100);
        count++;
    });
    EXPECT_EQ(count, 1);
}

TEST(ElysiaQueryEntity, EntityWithFilter) {
    World world;
    struct Tag {};
    
    auto e1 = world.spawn().add(Value{1}).add(Tag{}).entity;
    world.spawn().add(Value{2}); // No Tag

    int count = 0;
    world.query<Entity>()
         .filter<With<Tag>>()
         .each([&](Entity e) {
             EXPECT_EQ(e, e1);
             count++;
         });
    EXPECT_EQ(count, 1);
}
