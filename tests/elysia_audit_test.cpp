#include <gtest/gtest.h>
#include <memory_resource>

import elysia.mem;
import elysia.world;
import elysia.entity;
import elysia.core;

using namespace elysia;

struct TrackedComp {
    std::byte data[1024]; // Large enough to ensure physical allocation
};

TEST(ElysiaAudit, WorldPhysicalAudit) {
    // 1. Create a local audit resource
    AuditResource audit(get_default_allocator(), "ElysiaWorldAudit");
    
    {
        // 2. Create a world using this auditor
        World world(&audit);
        
        // 3. Do some structural changes
        auto e1 = world.spawn();
        e1.add(TrackedComp{});
        
        auto e2 = world.spawn();
        e2.add(TrackedComp{});
        
        e1.remove<TrackedComp>();
        
        // 4. World is about to be destroyed
    }
    
    // 5. If everything was cleared correctly (Table, Registry, etc.), 
    // AuditResource should be empty now.
    audit.report_leaks(); 
}

TEST(ElysiaAudit, CommandBufferPhysicalAudit) {
    AuditResource audit(get_default_allocator(), "CmdBufferAudit");
    
    {
        // Create an explicit shared ptr for the allocator to match Buffer signature
        auto alloc_ptr = std::shared_ptr<Allocator>(&audit, [](auto*){});
        EntityIndex index;
        CommandBuffer cmd(&index, alloc_ptr);
        
        Entity e(1, 0);
        cmd.insert(e, TrackedComp{});
        cmd.spawn(Entity(2, 0));
        
        // cmd out of scope
    }
    
    audit.report_leaks();
}