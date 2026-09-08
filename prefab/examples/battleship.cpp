#include "battleship_model.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace battleship_demo;
    try {
        auto prefab = prepare(read_file(argc > 1 ? argv[1] : "fixtures/battleship.json"));
        std::cout << "JSON loaded once; prepared " << prefab.entity_count() << "-entity native battleship.\n";
        World world;
        const std::array<SpawnParams, 2> parameters{{
            {"Resolute", {100, 630, 0}, 1}, {"Vanguard", {900, 630, 0}, 2}
        }};
        auto fleet = p::spawn_native_batch(world, prefab, std::span<const SpawnParams>(parameters));
        for (const auto& ship : fleet) {
            verify(world, ship);
            const auto& identity = *world.get_component<Identity>(ship.root());
            const auto& position = world.get_component<Transform>(ship.root())->position;
            std::cout << identity.callsign << ": faction " << identity.faction << ", x=" << position[0]
                      << ", " << ship.entities.size() << " entities\n";
            auto ids = records(world, ship);
            for (uint32_t local : {11u, 21u, 31u}) {
                const auto& gun = *world.get_component<Gun>(ids.at(local));
                std::cout << "  battery " << local << ": " << gun.barrels << " x " << gun.caliber_mm
                          << " mm; hull/radar references verified\n";
            }
        }
        world.get_component<Hull>(fleet[0].root())->hitpoints = 0;
        if (world.get_component<Hull>(fleet[1].root())->hitpoints <= 0)
            throw p::Error("Damage leaked between instances");
        std::cout << "Independent native instances verified; no JSON decoding during batch spawn.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
