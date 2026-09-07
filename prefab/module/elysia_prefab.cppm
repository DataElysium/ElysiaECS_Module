module;
#include <elysia/prefab/prefab.hpp>
export module elysia.prefab;

// All declarations retain global-module attachment and come from the headers.
export namespace elysia {
using ::elysia::World;
using ::elysia::Entity;
using ::elysia::TypeTraits;
}
export namespace elysia::archive {
using ::elysia::archive::SnapshotRegistry;
}
export namespace elysia::prefab {
using ::elysia::prefab::Value;
using ::elysia::prefab::Object;
using ::elysia::prefab::Array;
using ::elysia::prefab::Error;
using ::elysia::prefab::EntityRecord;
using ::elysia::prefab::PrefabClass;
using ::elysia::prefab::Library;
using ::elysia::prefab::Instance;
using ::elysia::prefab::Document;
using ::elysia::prefab::LookupScope;
using ::elysia::prefab::NameTable;
using ::elysia::prefab::ComponentRegistry;
using ::elysia::prefab::PrefabRegistry;
using ::elysia::prefab::Spawned;
using ::elysia::prefab::Local;
using ::elysia::prefab::NameTag;
using ::elysia::prefab::ChildOf;
using ::elysia::prefab::PrefabEntityId;
using ::elysia::prefab::Refs;
using ::elysia::prefab::Bound;
using ::elysia::prefab::Net;
using ::elysia::prefab::Pins;
using ::elysia::prefab::Use;
using ::elysia::prefab::PrefabRoot;
using ::elysia::prefab::Patch;
using ::elysia::prefab::parse_json;
using ::elysia::prefab::read_library;
using ::elysia::prefab::read_document;
using ::elysia::prefab::write_library;
using ::elysia::prefab::resolve_refs;
using ::elysia::prefab::lower_nets;
using ::elysia::prefab::apply_patch;
}
