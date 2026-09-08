# Elysia prefab

Header-only prefab authoring and instantiation for ElysiaECS. The wire format
and composition rules follow `bevy_dll_demo/libs/ecs_prefab`. No Jinja, scripting
runtime, renderer, event bus, or scheduler is required.

The implementation is in `include/elysia/prefab`. `reflect-cpp` remains a linked
dependency supplied by the existing Elysia archive layer; header-only describes
this library, not every third-party dependency.

## Build and run

Run these commands **inside this directory**. `-P .` is necessary: otherwise
xmake may discover and build the parent module project.

```sh
xmake f -P . --module=y -y
xmake build -P .
xmake run -P . prefab_tests
xmake run -P . prefab_car
xmake run -P . prefab_module_smoke
```

The default header dependency is `../../ElysiaECS_Hpp/include`. Override it with
`xmake f -P . --ecs_headers=/path/to/include`. No second copy of the ECS headers
is maintained here. Module support is optional and off by default.

The car example loads the unchanged Rust demo `game.json` and reports three
instances, eleven entities, one car, and four wheels. Four of the entities are
the original `Use` attachment points; expansion removes the instruction, not
those entities. Pass another manifest as the first argument. A second argument
`--dump` prints normalized component data and hierarchy paths for comparison.

To compare the real Rust and C++ implementations:

```sh
python3 tests/compare_rust.py /path/to/bevy_dll_demo
```

This compiles a temporary Rust executable using that repository's actual
`ecs_prefab`, customized Bevy, and shared component definitions. It compares all
registered car component values and hierarchy paths, excluding native entity
IDs. Floating-point comparison permits the Rust f32/C++ double representation
difference. It does not modify the Rust workspace.

## Native types and public names

```cpp
#include <elysia/prefab/prefab.hpp>

struct InductorInput { double henries; };

elysia::archive::SnapshotRegistry case_types;
case_types.register_type<InductorInput>("electrical::Inductor");

elysia::prefab::PrefabRegistry prefabs(case_types);
prefabs.load_library(elysia::prefab::read_library(
    elysia::prefab::parse_json(R"({
      "electrical::branch": {
        "params": {"L": 0.2},
        "body": [{"id": 0, "components": {
          "Inductor": {"henries": "$L"}
        }}]
      }
    })")));

elysia::World world;
auto instance = prefabs.spawn_class(world, "electrical::branch");
```

Native registration uses `TypeTraits<T>::id`, evaluated at compile time. A file
name resolves to a canonical registered name, then to the native ID and codec.
The authored name is never assumed to hash to the native ID. Different backends
may have different native IDs for the same public schema.

By default a component's lookup scope is the containing prefab namespace.
`component_scope(LookupScope)` can instead set an explicit application namespace,
imports, and aliases. Lookup order is: qualified exact name, current scope,
explicit alias, imported scopes, global name. Conflicting imports are errors.
The `::Name` spelling explicitly addresses a global component name. Names default
to the native full name only when the application supplies no registration name.

Prefabs and component names use separate symbol tables. Prefab `Use` lookup is
qualified exact, current prefab namespace, then global. No compiler namespace is
automatically imported. `PrefabRegistry(registry)` borrows an application-owned `SnapshotRegistry`.
Archive operations and multiple prefab libraries can use the same instance:

```cpp
elysia::archive::SnapshotRegistry archive;
elysia::prefab::PrefabRegistry game_prefabs(archive);
elysia::prefab::PrefabRegistry editor_prefabs(archive);

// Both libraries see registrations added after construction.
archive.register_type<InductorInput>("electrical::Inductor");

// Registration through prefab also updates that same archive.
game_prefabs.components().register_type<Transform>();
auto& shared_archive = game_prefabs.archive_registry(); // same object as archive
```

The borrowed archive must outlive every prefab library/view using it. Construction
adds any missing prefab builtin codecs to that archive. No global registry is used.
A default-constructed `ComponentRegistry` owns its storage; copying it shares that
storage, so passing it to multiple prefab libraries does not copy the registrations.
Moving a `SnapshotRegistry` into `ComponentRegistry` explicitly transfers ownership.
For isolation, copy the archive explicitly before passing it in.

Scoped lookup reads the current archive factories directly. There is no duplicated
name table to synchronize; later additions, renames, and conflicting names are
visible on the next lookup. This currently scans the registered component types
at authoring/instantiation time; it adds no lookup to running ECS systems.
Register or replace codecs only between prefab/archive operations, never during
active decoding (including inside a decoder or observer). Existing template-world
values are not retroactively rebuilt by registration changes; reload the library
when its typed template view must reflect a changed codec.

## Supported data behavior

- Flat records with local IDs and parent links; duplicate IDs and parent cycles
  are rejected.
- Declared defaults, required null parameters, deep object overrides, `$param`
  paths, and recursively resolved `@global` values.
- Recursive `Use`, scoped prefab names, and instance-local identity. Composition
  cycles are errors; nesting is limited to 128 classes.
- `Local`, `NameTag`, `Refs`, `Net`, and `Pins` use the Rust serde wire shape
  (string/map/array), not a C++ wrapper-object shape. Application newtypes can
  use `register_proxy<T, Wire>()` for the same purpose.
- `resolve_refs()` derives `Bound` from `$id:`, `$ref:`, and `@ref:`. Missing
  references are skipped by default, as in Rust; strict mode rejects them.
  Duplicate global or sibling names are rejected rather than resolved arbitrarily.
- `Patch` supports JSON deep-merge `set` and full-value `attach`. An absent target
  returns false. Unknown components and missing `set` components are errors.
- Optional net qualification and `lower_nets()` preserve symbolic nets while
  producing numeric pins. `@gnd` can refer to a registered global ground;
  literal internal names are scoped like the Rust spawn path.

Hierarchy reuses the ECS plugin's `elysia::ChildOf` and `elysia::Children` types;
the prefab names are aliases, not separate components. The shared implementation
is in `elysia/hierarchy.hpp` (`elysia.hierarchy` for module consumers).
`HierarchyPlugin` delegates to the same `install_hierarchy(World&)` function,
so a plain prefab world does not need an App or a scheduler.

Loading installs the hooks on the private template world. Spawning installs them
on the destination world before inserting validated parent links. Children lists
are populated immediately, including nested Use attachment points and records
whose parents appear later in the file. Record validation uses one graph-based
cycle check rather than repeatedly walking ancestors.

```cpp
auto instance = prefabs.spawn_class(world, "electrical::branch");
auto root = instance.roots.at(0);
auto entities = elysia::collect_subtree(world, root);
auto graph = elysia::build_hierarchy_graph(world, root);
// graph.key(node_id) gives the original Entity; components remain in world.

auto authored_graph = prefabs.template_graph("electrical::branch");
auto authored_root = prefabs.template_root("electrical::branch");
```

Both traversal functions follow Children within the selected subtree; neither
queries all ChildOf components. ChildOf/Children are the live relationship data.
The returned DirectedGraph is an optional snapshot, rebuilt on request after
editing; it is not another automatically maintained cache on the root.

Supported edits are `attach_child(world, parent, child)`,
`detach_child(world, child)`, and `reparent(world, child, new_parent)`.
Reparenting checks the child's descendants for cycles, then removes the old
ChildOf before inserting the new one. Same-parent attachment is idempotent.
`despawn_subtree(world, root)` deletes descendants before their parents;
ordinary world.despawn also cascades through installed hierarchy hooks.

Install hierarchy before independently inserting relationship components.
As with world-bound observer callbacks, keep that World at a stable address
until it is destroyed. Mutations require exclusive structural access. Direct
writes to relationship fields, direct replacement with a different parent
without removal, and direct edits to Children bypass this API's consistency
contract. Hierarchy codecs are not accepted as ordinary prefab component data;
parent links are expressed through record.parent.

Reference resolution remains explicit and rerunnable after entity changes.

## Authoring, archives, and failure boundaries

`load_library()` owns the original authored definitions and a separate typed
template world containing decodable defaults, hierarchy, and `Use` records.
Required parameter values may prevent a component default from materializing;
its authored expression is retained and decoded when instantiated.
`export_library()` / `export_class()` preserve those authored definitions.
They do not reverse gameplay mutations into prefab source. The template world
is read-only through this API; live-world boundary-port extraction from the Rust
library is not part of this initial port.

Case, runtime-state, and result registries remain application decisions. Only
explicitly registered types are decoded; serialization still uses the selected
archive registry. The derived `Bound` and `PrefabEntityId` data are not registered
for persistence by the prefab layer.

Expansion and decoding are preflighted in a private world. Bad names, parameters,
hierarchy, and component data do not create entities in the destination.
Destination insertion failures clean up newly created entities, but external
side effects from application observers cannot be rolled back. Custom codecs
must be pure component conversions: preflight invokes them before insertion.
Spawn, patch, and reference resolution require an exclusive structural access
point; no background scheduling is added by this library.

The current Generic representation follows Elysia's reflect-cpp codec (including
its signed 64-bit integer representation). A complete JSON-schema engine and
full parity with every Rust extraction API are not claimed.

## Module boundary

`module/elysia_prefab.cppm` includes the headers in its global module fragment
and re-exports public names. The module smoke test instantiates a user-defined
component from an importing translation unit. The ordinary tests include the
same headers from multiple translation units.

This wrapper exposes the **header-owned** Elysia types. Do not combine it with
the legacy module-owned `elysia::World` in one program. It demonstrates the
header-first module packaging; converting the old core module implementations
to that arrangement is a separate migration.

## Fixture provenance

- `fixtures/game.json`: unmodified copy of `bevy_dll_demo/game.json`.
- `fixtures/circuit.json`: unmodified JSON from Rust's
  `spawn_class_recursive_qualifies_internals_and_wires_ports` test.

The fixtures and compatibility helper exercise the existing Rust implementation;
they are not a claim of automatic checkpoint interchange between different solvers.

### Native templates and typed batch spawning

Include `elysia/prefab/native.hpp` for a native-only path; it has no archive,
reflection, JSON, or file-format dependency. Build an ordinary authoring world,
using the hierarchy API for its tree, then prepare an owned snapshot:

```cpp
struct ShipParams { int armor; };
void patch_ship(elysia::World& world, std::span<const elysia::Entity> slots,
                const ShipParams& params) {
    world.get_component<Hull>(slots[0])->armor = params.armor;
}

elysia::prefab::NativeCloneRegistry clones;
elysia::prefab::register_native_clone<Hull>(clones);
auto prefab = elysia::prefab::prepare_native_prefab(
    "ship", authoring_world, hull_entity, clones,
    elysia::prefab::native_parameters<ShipParams, patch_ship>());

std::array<ShipParams, 2> parameters{{{100}, {200}}};
auto instances = elysia::prefab::spawn_native_batch(
    destination, prefab, std::span<const ShipParams>(parameters));
```

`NativeComponentFunctions` contains a nullable clone function pointer.
`register_native_clone<T>` supplies copy construction; a custom callback can
clone move-only components. Components containing entity references must register
their own remapper, for example:

```cpp
void remap_turret(Turret& turret, const elysia::prefab::NativeEntityMap& map) {
    turret.hull = map.resolve(turret.hull);
}
elysia::prefab::register_native_clone<Turret, remap_turret>(clones);
```

The remapper runs on the copy before insertion. All destination entity IDs already
exist at that point, although their components may still be under construction.
Null references stay null; `resolve` rejects references outside the template tree.
Cross-world external references require an explicitly designed custom clone policy.

`NativePrefabFunctions` contains an optional patch function pointer and its
parameter type ID. Omit it for parameterless spawning with `spawn_native` or
`spawn_native_batch(world, prefab, count)`. Typed calls validate the parameter type
before spawning. Slots follow breadth-first hierarchy order, root first; they are
stable within a prepared snapshot, not identifiers for matching independently
authored files.

Preparation validates clone coverage, copies component values into a private
world, remaps references, and caches traversal and clone callbacks. The authoring
world and clone registry may subsequently change or be destroyed. The snapshot
does not retain pointers into either; callback code and any external resources
used by custom clones must remain available. Default copy semantics apply to
shared pointers and other shared resources. Custom callbacks should not mutate
their source.

Hierarchy components are rebuilt through the hierarchy plugin rather than copied.
Patching runs after the hierarchy is complete. Patches should modify the new
instance's values through ECS APIs and leave its entity membership unchanged.
If cloning or patching throws, created entities are removed; a failed batch removes
its earlier instances too. This cannot undo arbitrary callback side effects on
resources or other entities; removal observers must not throw during cleanup.

This is the native foundation: named file overrides, compatibility matching, and
Generic-to-typed parameter decoding are not yet connected to this path. Batch
spawning currently repeats the prepared native plan; it does not yet allocate or
copy whole archetype columns in bulk.


### JSON battleship to native template

Run `xmake build -P . prefab_battleship` then
`xmake run -P . prefab_battleship` from this prefab directory.
An optional first argument selects another JSON file.

`fixtures/battleship.json` defines an 11-entity tree: hull, three turret mounts
with triple 410 mm batteries, two Gatling mounts, radar, and engine.
`examples/battleship_model.hpp` registers the component decoders and native clone
callbacks. It loads the file, expands its default parameters, binds references,
and prepares an owned native snapshot. The source registry and authoring world
are destroyed before batch spawning.

The example spawns Resolute and Vanguard with typed callsign/faction/position
parameters. Clone callbacks remap both `Bound` references and `PrefabEntityId`
instance identities. Hierarchy is rebuilt by the native spawner. Repeated spawning
does not parse JSON or resolve symbolic references again.

This example explicitly prepares the loaded tree; it does not automatically
match a file to a separately registered native definition by name. It demonstrates
the loading/spawning boundary without changing PolarAegis gameplay or rendering.
