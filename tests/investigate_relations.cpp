#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

import elysia.world;
import elysia.query;
import elysia.meta;
import elysia.entity;

using namespace elysia;

// The "Heavy" Data
struct Mesh {
    std::string name;
    int vertices = 0;
};

// The Component Wrapper (just a shared_ptr)
struct SharedMesh {
    std::shared_ptr<Mesh> ptr;
    
    // Helper accessors
    Mesh* operator->() const { return ptr.get(); }
    Mesh& operator*() const { return *ptr; }
};

TEST(InvestigateRelations, SharedPointerInColumn) {
    World world;

    // 1. Create two shared resources (Managed by C++ RAII)
    auto mesh_A = std::make_shared<Mesh>("Dragon", 5000);
    auto mesh_B = std::make_shared<Mesh>("Cube", 8);

    // 2. Spawn entities mixing these resources
    // Notice: They all have the SAME component signature {SharedMesh, int}
    // So they should end up in the SAME Archetype.
    for(int i=0; i<10; ++i) {
        if (i % 2 == 0) {
            world.spawn().add(SharedMesh{mesh_A}).add(i);
        } else {
            world.spawn().add(SharedMesh{mesh_B}).add(i);
        }
    }

    // 3. Verify Archetype Consolidation
    // Expect 3 Archetypes: Root, {SharedMesh} (intermediate), {SharedMesh, int} (final)
    // Because .add() is sequential, it creates intermediate archetypes.
    EXPECT_GE(world.archetype_count(), 2); 
    
    // Crucial Check:
    // Querying for the full signature should match EXACTLY ONE archetype.
    // This proves that Mesh A and Mesh B entities live in the SAME table.
    Query<SharedMesh, int> q_full;
    world.update_query(q_full);
    EXPECT_EQ(q_full.archetypes().size(), 1);

    // 4. Query and Iterate
    int dragon_count = 0;
    int cube_count = 0;

    // Use the existing prepared query
    q_full.each([&](SharedMesh& m, int& val) {
        // Access shared data via ->
        if (m->name == "Dragon") {
            dragon_count++;
            EXPECT_EQ(m.ptr.use_count(), 6); // 1 local + 5 entities
        } else if (m->name == "Cube") {
            cube_count++;
            EXPECT_EQ(m.ptr.use_count(), 6); // 1 local + 5 entities
        }
    });

    EXPECT_EQ(dragon_count, 5);
    EXPECT_EQ(cube_count, 5);
}
