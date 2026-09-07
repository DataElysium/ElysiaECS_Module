#include "car_model.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        elysia::World world;
        elysia::prefab::PrefabRegistry registry(car_demo::registry());
        auto spawned = car_demo::load_game(world, registry, car_demo::read_file(argc > 1 ? argv[1] : "fixtures/game.json"));
        if (argc > 2 && std::string_view(argv[2]) == "--dump") {
            std::cout << elysia::reflect::write_json(car_demo::normalized(world, registry, spawned)) << '\n';
            return 0;
        }
        size_t wheels = 0, bodies = 0;
        world.query<const car_demo::CarWheel>().each([&](const auto&) { ++wheels; });
        world.query<elysia::With<car_demo::CarBody>, elysia::Entity>().each([&](auto) { ++bodies; });
        std::cout << "Rust prefab loaded into C++ ECS: " << spawned.roots.size() << " instances, "
                  << spawned.entities.size() << " entities, " << bodies << " car, " << wheels << " wheels\n";
        return wheels == 4 && bodies == 1 ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
