#include <gtest/gtest.h>
#include <iostream>

import elysia;

using namespace elysia;

// Use exactly the same structs as in the archive test
struct RealPos { 
    float x, y; 
    static constexpr auto elysia_name = "RealPos";
};

struct RealVel { 
    float dx, dy; 
    static constexpr auto elysia_name = "RealVel";
};

TEST(ElysiaSanity, ChainedAddCorruption) {
    World world;
    
    // 1. Create entity
    auto e = world.spawn();
    std::cout << "[1] Spawned E" << e.entity.id() << std::endl;

    // 2. First add
    e.add(RealPos{10.5f, 20.5f});
    auto* p1 = e.get<RealPos>();
    ASSERT_NE(p1, nullptr);
    std::cout << "[2] After add(Pos): x=" << p1->x << ", y=" << p1->y << std::endl;
    EXPECT_EQ(p1->x, 10.5f);

    // 3. Second add (The critical migration)
    e.add(RealVel{1.1f, 2.2f});
    
    // 4. Verify BOTH
    auto* p2 = e.get<RealPos>();
    auto* v2 = e.get<RealVel>();
    
    ASSERT_NE(p2, nullptr) << "Position lost after migration!";
    ASSERT_NE(v2, nullptr) << "Velocity not added!";
    
    std::cout << "[3] After add(Vel):" << std::endl;
    std::cout << "    Pos: x=" << p2->x << ", y=" << p2->y << std::endl;
    std::cout << "    Vel: dx=" << v2->dx << ", dy=" << v2->dy << std::endl;

    EXPECT_EQ(p2->x, 10.5f);
    EXPECT_EQ(p2->y, 20.5f);
    EXPECT_EQ(v2->dx, 1.1f);
    EXPECT_EQ(v2->dy, 2.2f);
}
