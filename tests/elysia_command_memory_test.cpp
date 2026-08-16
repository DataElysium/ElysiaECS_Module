#include <gtest/gtest.h>
#include <atomic>
#include <memory>

import elysia.world;
import elysia.meta;
import elysia.entity;

using namespace elysia;

// --- Static Monitor ---
static std::atomic<int> g_cmd_instances{0};

struct CmdSpy {
    int id;
    CmdSpy() : id(0) { g_cmd_instances++; }
    explicit CmdSpy(int i) : id(i) { g_cmd_instances++; }
    ~CmdSpy() { g_cmd_instances--; }
    
    CmdSpy(const CmdSpy& other) : id(other.id) { g_cmd_instances++; }
    CmdSpy(CmdSpy&& other) noexcept : id(other.id) { 
        other.id = -1; 
        g_cmd_instances++; 
    }
    
    static constexpr auto elysia_name = "CmdSpy";
};

TEST(ElysiaCommandMemory, RedundantInsertAudit) {
    g_cmd_instances = 0;
    World world;
    Entity e = world.spawn().entity;

    {
        CommandBuffer cmd(&world.index());
        
        // 1. Double insert of the SAME component type
        cmd.insert(e, CmdSpy{1});
        cmd.insert(e, CmdSpy{2});
        
        // Count should be 2 (both in buffer payload)
        EXPECT_EQ(g_cmd_instances.load(), 2);
        
        // 2. Submit
        world.submit(cmd);
        
        // After submit:
        // - 1 instance in World Table (CmdSpy{2})
        // - 2 instances in CommandBuffer (Moved-from CmdSpy{1} and CmdSpy{2})
        // Total should be 3 if everything is cleared correctly after the function.
        // But wait, submit() calls cmd.clear() which should destroy both in buffer.
        // So only World Table should remain.
        EXPECT_EQ(g_cmd_instances.load(), 1);
    }
    
    // 3. Cleanup world
    world = World();
    EXPECT_EQ(g_cmd_instances.load(), 0);
}

TEST(ElysiaCommandMemory, InterleavedOpsAudit) {
    g_cmd_instances = 0;
    World world;
    
    {
        CommandBuffer cmd(&world.index());
        Entity e1 = world.spawn().entity;
        
        cmd.insert(e1, CmdSpy{10});
        cmd.despawn(e1); // The insert should technically be "wasted"
        
        EXPECT_EQ(g_cmd_instances.load(), 1);
        
        world.submit(cmd);
        
        // After submit: e1 is gone, so the CmdSpy{10} moved into table must be destroyed.
        // The one in buffer must be destroyed by clear().
        EXPECT_EQ(g_cmd_instances.load(), 0);
    }
}