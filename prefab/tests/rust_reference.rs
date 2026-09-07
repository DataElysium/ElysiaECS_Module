use bevy_ecs::prelude::*;
use bevy_ecs::hierarchy::ChildOf;
use ecs_prefab::{Library, NameTag, PrefabEntityId, PrefabRegistry};
use shared_api::*;
use serde_json::{Value, Map};

fn path(world: &World, entity: Entity) -> String {
    if let Some(parent) = world.get::<ChildOf>(entity) {
        format!("{}/{}", path(world, parent.parent()), world.get::<PrefabEntityId>(entity).unwrap().local)
    } else {
        world.get::<NameTag>(entity).unwrap().0.clone()
    }
}
fn main() {
    let file = std::env::args().nth(1).unwrap();
    let doc: Value = serde_json::from_str(&std::fs::read_to_string(file).unwrap()).unwrap();
    let lib: Library = serde_json::from_value(doc["prefabs"].clone()).unwrap();
    let mut registry = PrefabRegistry::default();
    registry.register::<Transform>().register::<Visual>().register::<Spin>()
        .register::<Velocity>().register::<Gravity>().register::<PlayerControl>()
        .register::<Collider>().register::<CarBody>().register::<CarWheel>();
    registry.load_library(&lib).unwrap();
    let mut world = World::new();
    for instance in doc["instances"].as_array().unwrap() {
        let id = instance["id"].as_str().unwrap();
        let params = instance.get("params").and_then(Value::as_object).cloned().unwrap_or_default();
        let roots = registry.try_spawn_class(&lib, instance["prefab"].as_str().unwrap(), &params,
                                            &format!("{id}."), &mut world).unwrap();
        assert_eq!(roots.len(), 1);
        world.entity_mut(roots[0]).insert(NameTag(id.into()));
    }
    let entities: Vec<Entity> = world.query_filtered::<Entity, With<PrefabEntityId>>().iter(&world).collect();
    let mut result = Map::new();
    for entity in entities {
        let mut components = Map::new();
        for (name, factory) in &registry.archive_registry.entries {
            if let Some(value) = (factory.js_value.export)(&world, entity) {
                if *name != "ChildOf" { components.insert(name.to_string(), value); }
            }
        }
        result.insert(path(&world, entity), Value::Object(components));
    }
    println!("{}", Value::Object(result));
}
