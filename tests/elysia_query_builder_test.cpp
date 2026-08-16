#include <gtest/gtest.h>
#include <vector>

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

using namespace elysia;
namespace elysia_query_builder_test {
// Test Components
struct Position { float value; };
struct Velocity { float value; };
struct Renderable { }; // Empty Tag
struct Dead { };       // Empty Tag
}
using namespace elysia_query_builder_test;
TEST(ElysiaQueryBuilder, DynamicFilterAPI) {
    World world;

    // Entity 1: Has everything -> Should match
    auto e1 = world.spawn()
        .add(Position{10.0f})
        .add(Velocity{5.0f})
        .add(Renderable{})
        .entity;

    // Entity 2: Missing Renderable -> Should NOT match
    world.spawn()
        .add(Position{20.0f})
        .add(Velocity{2.0f});

    // Entity 3: Has Dead -> Should NOT match (due to Without<Dead>)
    world.spawn()
        .add(Position{30.0f})
        .add(Velocity{3.0f})
        .add(Renderable{})
        .add(Dead{});

    // Verify initial state
    auto* p1 = world.get_component<Position>(e1);
    EXPECT_EQ(p1->value, 10.0f);

    // THE API TEST:
    // world.query<Position, const Velocity>()
    //      .filter<With<Renderable>, Without<Dead>>()
    //      .each(...)
    
    int count = 0;
    world.query<Position, const Velocity>()
         .filter<With<Renderable>, Without<Dead>>()
         .each([&](Position& p, const Velocity& v) {
             p.value += v.value;
             count++;
         });

    // Validation
    EXPECT_EQ(count, 1); // Only e1 should match
    EXPECT_EQ(p1->value, 15.0f); // 10 + 5
}
