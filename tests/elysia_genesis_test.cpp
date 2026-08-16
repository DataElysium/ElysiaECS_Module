#include <gtest/gtest.h>
#include <vector>

import elysia.mem;
import elysia.entity;
import elysia.world;

TEST(ElysiaGenesis, EntityBitFields) {
    using namespace elysia;
    
    Entity e(100, 5);
    EXPECT_EQ(e.id(), 100);
    EXPECT_EQ(e.version(), 5);
    EXPECT_TRUE(e.is_valid());
    
    Entity null_e;
    EXPECT_FALSE(null_e.is_valid());
}

struct Position { float x, y; };

// TEST(ElysiaGenesis, CommandBufferFlow) {
//     using namespace elysia;
    
//     CommandBuffer cmd;
//     Entity e1(1, 1);
//     Position pos{10.0f, 20.0f};
    
//     cmd.insert(e1, pos);
    
//     auto headers = cmd.headers();
//     ASSERT_EQ(headers.size(), 1);
    
//     auto& h = headers[0];
//     EXPECT_EQ(h.op, OpCode::Insert);
//     EXPECT_EQ(h.entity.id(), 1);
    
//     // Triple-stream lookup
//     const ArgMeta* meta = cmd.get_meta(h.meta_index);
//     ASSERT_NE(meta, nullptr);
    
//     const std::byte* data_ptr = cmd.payload_data();
//     const Position* p_rec = reinterpret_cast<const Position*>(data_ptr + meta->payload_offset);
    
//     EXPECT_FLOAT_EQ(p_rec->x, 10.0f);
//     EXPECT_FLOAT_EQ(p_rec->y, 20.0f);
    
//     cmd.clear();
//     EXPECT_EQ(cmd.headers().size(), 0);
// }
