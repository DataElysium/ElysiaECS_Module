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

elysia::prefab::ComponentRegistry components(case_types);
elysia::prefab::PrefabRegistry prefabs(std::move(components));
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
automatically imported. Registry snapshots are copied into `ComponentRegistry`;
changes to the original registry do not silently change the loaded contract.

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

Hierarchy uses `elysia::prefab::ChildOf`, an ordinary component. This is the
prefab hierarchy contract, not an implicit dependency on a game hierarchy plugin.
Reference resolution is explicit and rerunnable after entity changes.

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
