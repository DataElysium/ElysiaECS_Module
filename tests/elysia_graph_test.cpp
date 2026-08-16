#include <gtest/gtest.h>
#include <vector>

import elysia.meta;
import elysia.storage;
import elysia.graph;
import elysia.config;

using namespace elysia;

struct Pos { float x, y; };
struct Vel { float x, y; };

TEST(ElysiaGraph, DedupAndRoot) {
    ArchetypeGraph<DefaultConfig> graph;
    
    auto* root = graph.root();
    EXPECT_EQ(root->types().size(), 0);
    
    const TypeInfo* t_pos = get_type_info_ptr<Pos>();
    std::vector<const TypeInfo*> types = { t_pos };
    
    auto* arch1 = graph.get_or_create(types);
    EXPECT_EQ(arch1->types().size(), 1);
    
    auto* arch2 = graph.get_or_create(types);
    EXPECT_EQ(arch1, arch2); // Dedup
}

TEST(ElysiaGraph, EdgeTraversal) {
    ArchetypeGraph<DefaultConfig> graph;
    auto* root = graph.root();
    
    const TypeInfo* t_pos = get_type_info_ptr<Pos>();
    
    auto* arch_pos = graph.traverse_add(root, *t_pos);
    EXPECT_EQ(arch_pos->types().size(), 1);
    EXPECT_EQ(arch_pos->types()[0]->id, TypeTraits<Pos>::id);
}

TEST(ElysiaGraph, CycleStability) {
    ArchetypeGraph<DefaultConfig> graph;
    auto* root = graph.root();
    const TypeInfo* t_pos = get_type_info_ptr<Pos>();
    
    auto* arch_pos = graph.traverse_add(root, *t_pos);
    auto* arch_back = graph.traverse_remove(arch_pos, t_pos->id);
    
    EXPECT_EQ(arch_back, root);
}