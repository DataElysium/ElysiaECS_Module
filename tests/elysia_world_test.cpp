#include <gtest/gtest.h>
#include <string>
#include <vector>

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

using namespace elysia;

struct Pos { float x, y; };
struct Vel { float dx, dy; };

TEST(ElysiaWorld, TheBigBang) {
    World world;
    auto e = world.spawn();
    
    e.add(Pos{10, 20});
    e.add(Vel{1, 1});
    
    Pos* p = e.get<Pos>();
    Vel* v = e.get<Vel>();
    
    ASSERT_NE(p, nullptr);
    ASSERT_NE(v, nullptr);
    EXPECT_FLOAT_EQ(p->x, 10);
    EXPECT_FLOAT_EQ(v->dx, 1);
}

TEST(ElysiaWorld, ComponentMoveSemantics) {
    struct Complex {
        std::string name;
        Complex(std::string n) : name(std::move(n)) {}
        Complex(Complex&&) = default;
        Complex& operator=(Complex&&) = default;
    };

    World world;
    auto e = world.spawn();
    e.add(Complex("Test"));
    
    Complex* c = e.get<Complex>();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->name, "Test");
}

TEST(ElysiaWorld, DespawnAndIndexRepair) {
    World world;
    auto e1 = world.spawn();
    auto e2 = world.spawn();
    
    e1.add(Pos{1, 1});
    e2.add(Pos{2, 2});
    
    e1.despawn();
    
    // Check if e2 is still valid and accessible
    Pos* p2 = e2.get<Pos>();
    ASSERT_NE(p2, nullptr);
    EXPECT_FLOAT_EQ(p2->x, 2);
}

TEST(ElysiaWorld, RemoveComponent) {
    World world;
    auto e = world.spawn();
    e.add(Pos{1, 1});
    e.add(Vel{2, 2});
    
    e.remove<Pos>();
    
    EXPECT_EQ(e.get<Pos>(), nullptr);
    EXPECT_NE(e.get<Vel>(), nullptr);
}

TEST(ElysiaWorld, QueryIteration) {
    World world;
    for(int i=0; i<100; ++i) {
        auto e = world.spawn();
        e.add(Pos{(float)i, 0});
        if(i % 2 == 0) e.add(Vel{1, 0});
    }
    
    Query<Pos, Vel> q;
    world.update_query(q);
    
    int count = 0;
    q.each([&](Pos& p, Vel& v) {
        p.x += v.dx;
        count++;
    });
    
    EXPECT_EQ(count, 50);
}