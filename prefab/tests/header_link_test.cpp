#include <elysia/prefab/prefab.hpp>
// A second translation unit catches non-inline header definitions at link time.
elysia::prefab::Value prefab_header_link_probe() { return elysia::prefab::parse_json("{}"); }
