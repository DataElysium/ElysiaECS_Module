#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <iostream>

import elysia.meta;
import elysia.entity;
import elysia.storage;
import elysia.core;
import elysia.mem;
import elysia.table;
import elysia.config;

using namespace elysia;

struct Label { 
    std::string text; 
    static constexpr auto elysia_name = "LabelComp";
};

TEST(ElysiaStorage, FullLifecycleWithIndex) {
    auto* alloc = get_default_allocator();
    EntityIndex index;
    
    const TypeInfo* info = get_type_info_ptr<Label>();
    std::vector<const TypeInfo*> types = { info };
    
    Archetype<DefaultConfig> arch(types, alloc, 100);
    auto modifier = arch.modify();
    
    std::vector<Entity> entities;
    for(int i=0; i<10; ++i) {
        Entity e = index.spawn();
        size_t row = modifier.push(e);
        auto loc = arch.table().locate(row);
        Label* l = static_cast<Label*>(loc.chunk->component(0, loc.index));
        l->text = "Entity_" + std::to_string(i);
        index.update(e, &arch, static_cast<uint32_t>(row));
        entities.push_back(e);
    }
    EXPECT_EQ(arch.count(), 10);
    
    Entity e5 = entities[5];
    Entity e_last = entities[9];
    auto rec5 = index.lookup(e5).unwrap();
    auto moved_entity = modifier.swap_remove(rec5->row);
    ASSERT_TRUE(moved_entity.has_value());
    EXPECT_EQ(moved_entity->id(), e_last.id());
    index.update(*moved_entity, rec5->archetype, rec5->row);
    index.free(e5);
    
    EXPECT_EQ(arch.count(), 9);
}

TEST(ElysiaStorage, PodOptimization) {
    struct Vec2 { float x, y; };
    auto* alloc = get_default_allocator();
    const TypeInfo* info = get_type_info_ptr<Vec2>();
    std::vector<const TypeInfo*> types = { info };
    Archetype<DefaultConfig> arch(types, alloc, 101);
    EXPECT_EQ(arch.types()[0]->hooks.move, nullptr);
}

TEST(ElysiaStorage, ReserveBehavior) {
    struct Tag {};
    auto* alloc = get_default_allocator();
    const TypeInfo* info = get_type_info_ptr<Tag>();
    std::vector<const TypeInfo*> types = { info };
    Archetype<DefaultConfig> arch(types, alloc, 102);
    
    // Adaptive check for Monolithic vs Paged
    const bool is_monolithic = (DefaultConfig::ChunkSize == -1);
    
    arch.table().reserve(2000);
    
    if (is_monolithic) {
        EXPECT_EQ(arch.table().chunks().size(), 1);
        EXPECT_GE(arch.table().chunks()[0]->capacity(), 2000);
    } else {
        size_t chunk_size = static_cast<size_t>(DefaultConfig::ChunkSize);
        size_t expected_chunks = (2000 + chunk_size - 1) / chunk_size;
        EXPECT_EQ(arch.table().chunks().size(), expected_chunks); 
    }
}

TEST(ElysiaStorage, ZeroSizeTag) {
    struct MyTag {};
    static_assert(std::is_empty_v<MyTag>);
    
    auto* alloc = get_default_allocator();
    const TypeInfo* info = get_type_info_ptr<MyTag>();
    std::vector<const TypeInfo*> types = { info };
    
    // ChunkSize is 16384. Entity is 8 bytes.
    // If Tag size is 0: Total size per row = 8.
    // If Tag size is 1: Total size per row = 8 + 1 (+ padding) = 9 or more.
    
    Archetype<DefaultConfig> arch(types, alloc, 1);
    auto layout = Chunk::calculate_layout(types, 100);
    
    size_t entity_payload = 100 * sizeof(Entity);
    size_t aligned_payload = (entity_payload + 63) & ~63;
    
    // If Tag size is 0, total size should match entity payload aligned.
    // If Tag size is 1, it would be significantly larger (at least +100 bytes).
    EXPECT_EQ(layout.total_size, aligned_payload);
    
    // Verify offsets
    EXPECT_EQ(layout.component_offsets.size(), 1);
    // Offset should be equal to entity array end (no extra space)
    EXPECT_EQ(layout.component_offsets[0], 100 * sizeof(Entity)); 
    // Wait, align? Entity is 8 bytes aligned.
    // Tag alignment is 1.
    // So offset is just end of entities.
}
