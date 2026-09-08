// Reviewable prototype: no new World APIs and no reflection dependency.
// This is an in-process example, not yet an exported C ABI or a DLL loader.
#include <algorithm>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>
import elysia;

namespace runtime_demo {
using namespace elysia;

// External metadata. Copy constructs an independent value into uninitialized storage.
// Destroy releases the value's owned resources; the host frees the outer storage.
struct RuntimeType {
    uint64_t id;
    const char* name;
    size_t size, alignment;
    void (*destroy)(void*) = nullptr;
    void (*move)(void*, void*) = nullptr;
    void (*copy)(void*, const void*) = nullptr;
};
struct ValueDeleter {
    RuntimeType type;
    void operator()(void* value) const {
        if (type.destroy) type.destroy(value);
        ::operator delete(value, std::align_val_t(type.alignment));
    }
};
using OwnedValue = std::unique_ptr<void, ValueDeleter>;

OwnedValue copy_value(const RuntimeType& type, const void* source) {
    if (!type.copy || !source) throw std::invalid_argument("Copy callback and source are required");
    void* memory = ::operator new(type.size, std::align_val_t(type.alignment));
    try {
        type.copy(memory, source);
    } catch (...) {
        ::operator delete(memory, std::align_val_t(type.alignment));
        throw;
    }
    return OwnedValue(memory, ValueDeleter{type});
}

// One host-owned resource stores the adapter's external maps. Recreating the
// adapter preserves registrations and values because they belong to the world.
struct RuntimeState {
    std::unordered_map<uint64_t, RuntimeType> types;
    std::unordered_map<uint64_t, OwnedValue> resources;
};
struct RuntimeTrait {
    World* world;

    RuntimeState& state() const {
        return *world->resources().get_or_create<RuntimeState>();
    }
    void register_type(RuntimeType type) const {
        if (!type.name || !type.size || !type.alignment ||
            (type.alignment & (type.alignment - 1)) || type.size % type.alignment)
            throw std::invalid_argument("Invalid runtime layout");
        // This prototype requires movement for component insertion.
        if (!type.move) throw std::invalid_argument("Move callback is required");
        auto& registry = world->graph().registry();
        if (registry.get_info(type.id)) throw std::invalid_argument("Type ID already registered");
        auto* info = registry.register_opaque(type.id, type.name, type.size, type.alignment);
        info->hooks.dtor = type.destroy;
        info->hooks.move = type.move;
        state().types.emplace(type.id, type);
    }
    Entity spawn() const { return world->spawn().entity; }
    void copy_component(Entity entity, uint64_t id, const void* source) const {
        const auto& type = state().types.at(id);
        auto temporary = copy_value(type, source);
        world->add_component_dynamic(entity, world->graph().registry().get_info(id), temporary.get());
        // The ECS has moved the value. Destroy the moved-from temporary.
    }
    void remove_component(Entity entity, uint64_t id) const {
        world->remove_component_dynamic(entity, id);
    }
    void copy_resource(uint64_t id, const void* source) const {
        auto& storage = state();
        auto value = copy_value(storage.types.at(id), source);
        storage.resources.insert_or_assign(id, std::move(value));
    }
    void* get_resource(uint64_t id) const {
        auto& resources = state().resources;
        auto it = resources.find(id);
        return it == resources.end() ? nullptr : it->second.get();
    }
    void remove_resource(uint64_t id) const { state().resources.erase(id); }
    void query(DynamicQuery& query, void (*visitor)(void*, const DynamicChunkView&), void* context) const {
        world->update_query(query);
        query.each_chunk(visitor, context);
    }
};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Only this namespace knows the component layout. It models code owned by a plugin.
namespace plugin {
constexpr auto samples_id = fnv1a_64("demo::Samples");
inline int live_buffers = 0;
inline int copies = 0;
struct Samples {
    std::unique_ptr<double[]> values;
    size_t count;
    Samples(const double* source, size_t count)
        : values(std::make_unique<double[]>(count)), count(count) {
        std::copy_n(source, count, values.get());
        ++live_buffers;
    }
    Samples(Samples&&) noexcept = default;
    Samples(const Samples&) = delete;
    ~Samples() { if (values) --live_buffers; }
};
RuntimeType samples_type() {
    return {
        samples_id, "demo::Samples", sizeof(Samples), alignof(Samples),
        [](void* value) { static_cast<Samples*>(value)->~Samples(); },
        [](void* destination, void* source) {
            new (destination) Samples(std::move(*static_cast<Samples*>(source)));
        },
        [](void* destination, const void* source) {
            const auto& value = *static_cast<const Samples*>(source);
            new (destination) Samples(value.values.get(), value.count);
            ++copies;
        }};
}
void populate(RuntimeTrait runtime, Entity first, Entity second) {
    const double readings[]{10, 20};
    Samples source(readings, 2);
    runtime.copy_component(first, samples_id, &source);
    runtime.copy_component(second, samples_id, &source);
    source.values[0] = -999; // Copies must remain independent.
    const double offsets[]{1, 2};
    Samples resource(offsets, 2);
    runtime.copy_resource(samples_id, &resource);
    resource.values[0] = -999;
}
void add_offsets(void* resource, const DynamicChunkView& chunk) {
    const auto& offsets = *static_cast<const Samples*>(resource);
    auto* samples = static_cast<Samples*>(chunk.columns[0].data);
    for (size_t i = 0; i < chunk.entities.size(); ++i) {
        require(samples[i].count == offsets.count, "Mismatched sample counts");
        for (size_t j = 0; j < offsets.count; ++j) samples[i].values[j] += offsets.values[j];
    }
}
struct Inspection { size_t entities = 0; const double* previous = nullptr; };
void inspect(void* context, const DynamicChunkView& chunk) {
    auto& result = *static_cast<Inspection*>(context);
    const auto* samples = static_cast<const Samples*>(chunk.columns[0].data);
    for (size_t i = 0; i < chunk.entities.size(); ++i) {
        require(samples[i].values[0] == 11 && samples[i].values[1] == 22, "Incorrect copied data");
        require(samples[i].values.get() != result.previous, "Copies share owned memory");
        result.previous = samples[i].values.get();
        ++result.entities;
        std::cout << "entity " << chunk.entities[i].id() << ": [11, 22]\n";
    }
}
} // namespace plugin
} // namespace runtime_demo

int main() {
    using namespace runtime_demo;
    {
        World world;
        RuntimeTrait runtime{&world};
        runtime.register_type(plugin::samples_type());

        auto first = runtime.spawn();
        auto second = runtime.spawn();
        plugin::populate(runtime, first, second);

        // Host code uses IDs, opaque resource pointers, and callback functions.
        // The same ID can identify a component and a world resource independently.
        RuntimeTrait another_view{&world};
        DynamicQuery query({.columns = {plugin::samples_id}});
        another_view.query(query, plugin::add_offsets, another_view.get_resource(plugin::samples_id));
        plugin::Inspection inspection;
        runtime.query(query, plugin::inspect, &inspection);
        require(inspection.entities == 2, "Expected two component instances");
        require(plugin::copies == 3 && plugin::live_buffers == 3, "Incorrect ownership accounting");

        runtime.remove_component(first, plugin::samples_id);
        require(plugin::live_buffers == 2, "Component destruction failed");
        runtime.remove_resource(plugin::samples_id);
        require(!runtime.get_resource(plugin::samples_id), "Resource removal failed");
        require(plugin::live_buffers == 1, "Resource destruction failed");
        // World destruction releases the second entity's buffer.
    }
    require(plugin::live_buffers == 0, "Owned buffer leaked");
    std::cout << "3 custom deep copies; component/resource/world cleanup passed.\n";
}
