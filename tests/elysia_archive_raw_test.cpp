#include <gtest/gtest.h>
#include <vector>
#include <string>

import elysia;
import elysia.archive;
using namespace elysia;
using namespace elysia::archive;

namespace raw_test {
    struct PodPos { float x, y; static constexpr auto elysia_name = "raw_test::PodPos"; };
    struct Velocity { float dx, dy; static constexpr auto elysia_name = "raw_test::Velocity"; };
    
    // 🌸 A non-POD component (has destructor)
    struct NonPodString { 
        std::string s; 
        static constexpr auto elysia_name = "raw_test::NonPodString"; 
    };
}
using namespace raw_test;

TEST(ElysiaArchiveRaw, PodSafetyCheck) {
    World world;
    world.spawn().add(PodPos{1, 2}).add(NonPodString{"dangerous"});

    // 1. Default pack should FAIL due to NonPodString
    auto res = RawArchive::pack(world);
    EXPECT_TRUE(res.is_err());
    EXPECT_EQ(res.error().code, ErrorCode::InvalidOperation);
    // Error message should mention the offending component
    EXPECT_NE(res.error().message.find("NonPodString"), std::string::npos);

    // 2. Unchecked pack should SUCCEED (but it's dangerous!)
    auto res_unchecked = RawArchive::pack(world, /*unchecked=*/true);
    EXPECT_TRUE(res_unchecked.is_ok());
    EXPECT_GT(res_unchecked.unwrap().size(), 0);
}

TEST(ElysiaArchiveRaw, WorldWildRoundtrip) {
    std::vector<char> buffer;
    Entity e1_original, e2_original, e_dead_original;

    {
        World world;
        
        // 1. Create and delete to bump version
        auto ent_temp = world.spawn().entity;
        e_dead_original = ent_temp;
        world.despawn(ent_temp);

        // 2. Create living entities
        e1_original = world.spawn().add(PodPos{10.0f, 20.0f}).entity;
        e2_original = world.spawn().add(PodPos{30.0f, 40.0f}).add(Velocity{1.0f, 1.0f}).entity;

        // 3. Pack the world
        auto pack_res = RawArchive::pack(world);
        ASSERT_TRUE(pack_res.is_ok()) << pack_res.error().message;
        buffer = std::move(pack_res.unwrap());
        ASSERT_GT(buffer.size(), 0);
    }

    {
        World world2;
        
        // 4. THE ULTIMATE ELEGANCE: Unpack without ANY template parameters!
        // The loader will auto-register PodPos and Velocity as opaque PODs
        // based on the metadata in the raw file.
        auto res = RawArchive::unpack(world2, buffer);
        ASSERT_TRUE(res.is_ok()) << res.error().message;

        // 5. Verify Skeleton (Entity Index)
        EXPECT_FALSE(world2.index().is_alive(e_dead_original));
        EXPECT_TRUE(world2.index().is_alive(e1_original));
        EXPECT_TRUE(world2.index().is_alive(e2_original));

        // 6. Verify Meat (Component Data)
        // Since world2 now has PodPos registered (dynamically), 
        // world2.entity(e1).get<PodPos>() will find the ID match.
        auto v1 = world2.entity(e1_original);
        ASSERT_NE(v1.get<PodPos>(), nullptr);
        EXPECT_EQ(v1.get<PodPos>()->x, 10.0f);

        auto v2 = world2.entity(e2_original);
        ASSERT_NE(v2.get<PodPos>(), nullptr);
        ASSERT_NE(v2.get<Velocity>(), nullptr);
        EXPECT_EQ(v2.get<PodPos>()->x, 30.0f);
        EXPECT_EQ(v2.get<Velocity>()->dx, 1.0f);
    }
}
