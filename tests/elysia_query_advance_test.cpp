#include <gtest/gtest.h>
#include <vector>

import elysia.bitset;
import elysia.query;
import elysia.meta;
import elysia.world;
import elysia.entity;

using namespace elysia;

struct Pos { float x, y; };
struct Vel { float x, y; };
struct Player {};
struct Dead {};
struct GlobalConfig { float gravity = 9.8f; };

// --- Feature 1: Explicit Query with Mix of Args ---
TEST(ElysiaQuery4_7, MixedArguments) {
    World world;
    world.resources().add(GlobalConfig{1.6f});
    
    // Entity 1: Match
    world.spawn().add(Pos{0, 10}).add(Vel{0, -1}).add(Player{});
    // Entity 2: No Player tag -> Blocked
    world.spawn().add(Pos{0, 20}).add(Vel{0, -1});
    // Entity 3: Has Dead tag -> Blocked
    world.spawn().add(Pos{0, 30}).add(Vel{0, -1}).add(Player{}).add(Dead{});

    Query<Pos, With<Player>, Res<GlobalConfig>, Without<Dead>, Vel> q;
    world.update_query(q);

    int count = 0;
    q.each(&world, [&](Pos& p, GlobalConfig& cfg, Vel& v) {
        EXPECT_EQ(p.y, 10);
        EXPECT_FLOAT_EQ(cfg.gravity, 1.6f);
        count++;
    });
    EXPECT_EQ(count, 1);
}

// --- Feature 2: make_query Automatic Deduction ---
TEST(ElysiaQuery4_7, AutoDeduction) {
    World world;
    world.resources().add(GlobalConfig{5.0f});
    world.spawn().add(Pos{1, 1}).add(Vel{10, 10});

    auto q = make_query([](Pos& p, const Vel& v, Res<GlobalConfig> cfg) {
        p.x += v.x * cfg->gravity;
    });
    
    world.update_query(q);
    q.each(&world, [](Pos& p, const Vel& v, GlobalConfig& cfg) {
        p.x += v.x * cfg.gravity;
    });

    auto* p = world.get_component<Pos>(Entity(0, 0));
    EXPECT_FLOAT_EQ(p->x, 1.0f + 10.0f * 5.0f);
}

// --- Feature 3: Const-Correctness & References ---
TEST(ElysiaQuery4_7, ConstCorrectness) {
    World world;
    world.spawn().add(Pos{100, 100});

    Query<const Pos> q;
    world.update_query(q);

    q.each([](const Pos& p) {
        EXPECT_EQ(p.x, 100);
        // p.x = 200; // This would fail compilation if uncommented
    });
}