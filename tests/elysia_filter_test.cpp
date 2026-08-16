#include <gtest/gtest.h>
#include <string>

import elysia;

using namespace elysia;

namespace query_test {
    struct NonEmptyA { 
        int val; 
        std::string name; // 非 POD
    };
    struct NonEmptyB { 
        float data; 
    };
}

TEST(ElysiaQuery, WithWithoutNonEmptyFilter) {
    using namespace query_test;
    World world;

    // 实体 1: 两者都有
    auto e1 = world.spawn()
        .add(NonEmptyA{ 1, "both" })
        .add(NonEmptyB{ 1.0f })
        .entity;

    // 实体 2: 只有 A
    auto e2 = world.spawn()
        .add(NonEmptyA{ 2, "only_a" })
        .entity;

    // 1. 测试 With<NonEmptyB>
    {
        auto q = world.query<NonEmptyA>().filter<With<NonEmptyB>>();
        int count = 0;
        q.each([&](NonEmptyA& a) {
            EXPECT_EQ(a.name, "both");
            count++;
        });
        EXPECT_EQ(count, 1);
    }

    // 2. 测试 Without<NonEmptyB>
    {
        auto q = world.query<NonEmptyA>().filter<Without<NonEmptyB>>();
        int count = 0;
        q.each([&](NonEmptyA& a) {
            EXPECT_EQ(a.name, "only_a");
            count++;
        });
        EXPECT_EQ(count, 1);
    }
}

namespace lifecycle_query_test {
    struct Pos { int value; };
    struct Vel { int value; };
}

TEST(ElysiaQuery, DefaultSkipsDisabledTag) {
    using namespace lifecycle_query_test;
    World world;

    auto active = world.spawn().add(Pos{1}).add(Vel{10}).entity;
    auto disabled = world.spawn().add(Pos{2}).add(Vel{20}).add(DisabledTag{}).entity;

    int count = 0;
    world.query<Pos, Vel>().each([&](Pos& p, Vel& v) {
        p.value += v.value;
        count++;
    });

    EXPECT_EQ(count, 1);
    EXPECT_EQ(world.entity(active).get<Pos>()->value, 11);
    EXPECT_EQ(world.entity(disabled).get<Pos>()->value, 2);
}

TEST(ElysiaQuery, DisabledTagCanBeExplicitlyIncluded) {
    using namespace lifecycle_query_test;
    World world;

    world.spawn().add(Pos{1}).add(Vel{10});
    world.spawn().add(Pos{2}).add(Vel{20}).add(DisabledTag{});

    int disabled_count = 0;
    world.query<Pos>().filter<With<DisabledTag>>().each([&](Pos& p) {
        EXPECT_EQ(p.value, 2);
        disabled_count++;
    });
    EXPECT_EQ(disabled_count, 1);

    int include_disabled_count = 0;
    world.query<Pos, Include<DisabledTag>>().each([&](Pos&) {
        include_disabled_count++;
    });
    EXPECT_EQ(include_disabled_count, 2);

    int include_inactive_count = 0;
    world.query<Pos, IncludeInactive>().each([&](Pos&) {
        include_inactive_count++;
    });
    EXPECT_EQ(include_inactive_count, 2);
}
