#include <utility>
import elysia.prefab;
struct ModuleValue { int value; };
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
    return world.get_component<ModuleValue>(instance.roots.at(0))->value == 42 ? 0 : 1;
}
