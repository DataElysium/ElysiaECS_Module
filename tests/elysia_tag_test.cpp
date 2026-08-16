#include <gtest/gtest.h>
#include <vector>
#include <algorithm>

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

using namespace elysia;

struct Pos { float x; };
struct TagA {};
struct TagB {};

TEST(ElysiaTag, ArchetypeMatching) {
    World world;

    // 1. Create 4 Distinct Archetypes
    // Arch 1: {Pos}
    world.spawn().add(Pos{1}); 
    
    // Arch 2: {Pos, TagA}
    world.spawn().add(Pos{2}).add(TagA{});
    
    // Arch 3: {Pos, TagB}
    world.spawn().add(Pos{3}).add(TagB{});
    
    // Arch 4: {Pos, TagA, TagB}
    world.spawn().add(Pos{4}).add(TagA{}).add(TagB{});

    // Ensure architecture graph is populated
    EXPECT_GE(world.archetype_count(), 4);

    // 2. Test: With<TagA>
    {
        Query<Pos, With<TagA>> q;
        world.update_query(q);
        
        const auto& archs = q.archetypes();
        EXPECT_EQ(archs.size(), 2); // Should match Arch 2 and Arch 4
        
        int count = 0;
        q.each([&](Pos& p) { count++; });
        EXPECT_EQ(count, 2);
    }

    // 3. Test: Without<TagB>
    {
        Query<Pos, Without<TagB>> q;
        world.update_query(q);
        
        const auto& archs = q.archetypes();
        EXPECT_EQ(archs.size(), 2); // Should match Arch 1 and Arch 2
        
        int sum = 0;
        q.each([&](Pos& p) { sum += (int)p.x; });
        // p=1 (Arch1) + p=2 (Arch2) = 3
        EXPECT_EQ(sum, 3);
    }

    // 4. Test: With<TagA> AND Without<TagB>
    {
        Query<Pos, With<TagA>, Without<TagB>> q;
        world.update_query(q);
        
        const auto& archs = q.archetypes();
        ASSERT_EQ(archs.size(), 1); // Should ONLY match Arch 2
        
        // Inspect the matched archetype directly
        auto* arch = archs[0];
        EXPECT_TRUE(arch->has<TagA>());
        EXPECT_FALSE(arch->has<TagB>());
        
        int count = 0;
        q.each([&](Pos& p) { 
            EXPECT_EQ(p.x, 2.0f);
            count++; 
        });
        EXPECT_EQ(count, 1);
    }
}

TEST(ElysiaTag, ZeroCostCheck) {
    // Verify that Tags do not appear in the DataTuple (no pointers fetched)
    using Q = Query<Pos, With<TagA>>;
    static_assert(Q::DataCount == 1, "TagA should be filtered out of DataTuple");
    static_assert(std::is_same_v<std::tuple_element_t<0, Q::DataTuple>, Pos>, "Only Pos should remain");
}
