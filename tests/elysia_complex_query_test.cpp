#include <gtest/gtest.h>
#include <string>
#include <vector>

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

using namespace elysia;
namespace complex_query{
struct TagA { int val; };
struct TagB { int val; };
struct TagC { int val; };
}
using namespace complex_query;

TEST(ElysiaComplexQuery, MultiArchetypeDataIntegrity) {
    World world;
    
    // 1. Create a mixture of archetypes
    // Entites 0-99: {TagA, TagB}
    // Entites 100-199: {TagB, TagC}
    // Entites 200-299: {TagA, TagB, TagC}
    
    for(int i=0; i<100; ++i) {
        world.spawn().add(TagA{i}).add(TagB{i * 2});
    }
    for(int i=100; i<200; ++i) {
        world.spawn().add(TagB{i * 2}).add(TagC{i * 3});
    }
    for(int i=200; i<300; ++i) {
        world.spawn().add(TagA{i}).add(TagB{i * 2}).add(TagC{i * 3});
    }
    
    // 2. Query <TagA, TagB>: Expect 200 entities (0-99 and 200-299)
    {
        Query<TagA, TagB> q;
        world.update_query(q);
        int count = 0;
        q.each([&](TagA& a, TagB& b) {
            EXPECT_EQ(a.val * 2, b.val);
            count++;
        });
        EXPECT_EQ(count, 200);
    }
    
    // 3. Query <TagB, TagC>: Expect 200 entities (100-299)
    {
        Query<TagB, TagC> q;
        world.update_query(q);
        int count = 0;
        q.each([&](TagB& b, TagC& c) {
            // b.val = i*2, c.val = i*3 => c = b * 1.5
            EXPECT_EQ(b.val * 3, c.val * 2);
            count++;
        });
        EXPECT_EQ(count, 200);
    }
}

TEST(ElysiaComplexQuery, CompileTimeUniquenessAssert) {
    // This test is mostly for manual verification of static_assert.
    // Uncommenting the next line should cause a compile error.
    // Query<TagA, TagA> q; 
}

TEST(ElysiaComplexQuery, StructuralMigrationDuringQuery) {
    World world;
    std::vector<Entity> entities;
    for(int i=0; i<100; ++i) {
        entities.push_back(world.spawn().add(TagA{i}).entity);
    }
    
    // Query TagA, then migrate some to TagB
    Query<TagA> q;
    world.update_query(q);
    
    // Migration
    for(int i=0; i<50; ++i) {
        world.entity(entities[i]).add(TagB{i});
    }
    
    // Re-sync query
    world.update_query(q);
    
    int count = 0;
    q.each([&](TagA& a) {
        count++;
    });
    // Still 100, but they are now in two different archetypes {TagA} and {TagA, TagB}
    EXPECT_EQ(count, 100);
}
