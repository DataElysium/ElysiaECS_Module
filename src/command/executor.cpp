module;
#include <vector>
#include <cstring>
#include <algorithm>
#include <span>
#include <memory>
#include <cassert>
#include <stdexcept>

module elysia.world;
import elysia.observer; 

namespace elysia {

void execute_buffer_internal(World& world, CommandBuffer& input, CommandBuffer& output) {
    auto headers = input.headers();
    size_t i = 0;
    size_t n = headers.size();
    
    std::vector<const TypeInfo*> fused_types;
    std::vector<void*> fused_data;
    fused_types.reserve(16);
    fused_data.reserve(16);

    while (i < n) {
        const auto& h = headers[i];
        switch (h.op) {
            case OpCode::Spawn: {
                Entity target = h.entity;
                size_t lookahead = i + 1;
                fused_types.clear(); fused_data.clear();
                
                while (lookahead < n) {
                    const auto& next_h = headers[lookahead];
                    if (next_h.op == OpCode::Insert && next_h.entity == target) {
                        const auto* meta = input.get_meta(next_h.meta_index);
                        assert(meta);
                        const auto* info = static_cast<const TypeInfo*>(meta->metadata);
                        
                        auto it_dec = world.decorators_.find(info->id);
                        if (it_dec != world.decorators_.end()) {
                            it_dec->second(world, output, target, input.get_payload(*meta));
                        }

                        fused_types.push_back(info);
                        fused_data.push_back(input.get_payload(*meta));
                        lookahead++;
                    } else break;
                }

                if (fused_types.empty()) {
                    world.spawn_at(target);
                } else {
                    world.batch().spawn_bundle(target, fused_types, fused_data);
                }
                i = lookahead;
                break;
            }

            case OpCode::Insert: {
                const auto* meta = input.get_meta(h.meta_index);
                if (meta) {
                    const auto* info = static_cast<const TypeInfo*>(meta->metadata);
                    
                    auto it_dec = world.decorators_.find(info->id);
                    if (it_dec != world.decorators_.end()) {
                        it_dec->second(world, output, h.entity, input.get_payload(*meta));
                    }

                    world.add_component_dynamic(h.entity, info, input.get_payload(*meta));
                }
                i++;
                break;
            }

            case OpCode::Despawn: {
                world.despawn(h.entity);
                i++;
                break;
            }

            case OpCode::DespawnId: {
                world.despawn(h.entity.id());
                i++;
                break;
            }

            case OpCode::Call: {
                const auto* meta = input.get_meta(h.meta_index);
                if (meta && meta->runner) {
                    meta->runner(&world, input.get_payload(*meta));
                }
                i++;
                break;
            }

            default:
                i++;
                break;
        }
    }
}

void World::submit(CommandBuffer& cmd) {
    const int MAX_ITERATIONS = 32; 
    
    CommandBuffer* pass_output = buffer_a_.get();
    if (&cmd == buffer_a_.get()) {
        pass_output = buffer_b_.get();
    }

    pass_output->reset(); 
    execute_buffer_internal(*this, cmd, *pass_output);
    cmd.reset(); 

    if (pass_output == buffer_b_.get()) {
        std::swap(buffer_a_, buffer_b_);
    }

    int iterations = 0;
    while (!buffer_a_->headers().empty() && iterations < MAX_ITERATIONS) {
        buffer_b_->reset();
        execute_buffer_internal(*this, *buffer_a_, *buffer_b_);
        buffer_a_->reset();
        std::swap(buffer_a_, buffer_b_);
        iterations++;
    }

    if (iterations >= MAX_ITERATIONS) {
        throw std::runtime_error("Elysia World Settlement Collapse: Cascade detected in decorators.");
    }

    // Consolidated ID pool cleanup after all cascades are finished.
    index_.cleanup_recycled_pool();
}

} // namespace elysia
