#include <utility>
#include <span>
import elysia.prefab;
struct ModuleValue { int value; };
struct ModuleParams { int value; };
void patch_native(elysia::World& world, std::span<const elysia::Entity> entities, const ModuleParams& params) {
    world.get_component<ModuleValue>(entities[0])->value = params.value;
}
int main() {
    elysia::prefab::ComponentRegistry types;
    types.register_type<ModuleValue>("demo::Value");
    elysia::prefab::PrefabRegistry prefabs(std::move(types));
    prefabs.load_library(elysia::prefab::read_library(elysia::prefab::parse_json(
        R"({"demo::sample":{"body":[{"id":0,"components":{"Value":{"value":42}}}]}})")));
    elysia::World world;
    auto instance = prefabs.spawn_class(world, "demo::sample");
    auto hierarchy = elysia::build_hierarchy_graph(world, instance.roots.at(0));
    if (hierarchy.node_count() != 1) return 2;
    if (world.get_component<ModuleValue>(instance.roots.at(0))->value != 42) return 1;
    elysia::prefab::NativeCloneRegistry clones;
    elysia::prefab::register_native_clone<ModuleValue>(clones);
    auto native = elysia::prefab::prepare_native_prefab(
        "native", world, world.spawn().add(ModuleValue{42}).entity, clones,
        elysia::prefab::native_parameters<ModuleParams, patch_native>());
    elysia::World destination;
    auto clone = elysia::prefab::spawn_native(destination, native, ModuleParams{73});
    return destination.get_component<ModuleValue>(clone.root())->value == 73 ? 0 : 3;
}
