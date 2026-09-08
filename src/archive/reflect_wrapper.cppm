module; 
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <rfl/toml.hpp>
#include <rfl/msgpack.hpp>
#ifdef ELYSIA_ENABLE_CAPNPROTO
#include <rfl/capnproto.hpp>
#endif
#include <rfl/Generic.hpp>
#include <rfl/to_generic.hpp>
#include <rfl/from_generic.hpp>
#include <string> 

export module elysia.reflect_wrapper;

export namespace elysia::reflect {
    using Generic = rfl::Generic;

    template<typename T> inline Generic to_generic(const T& obj) { return rfl::to_generic(obj); }
    template<typename T> inline auto from_generic(const Generic& value) { return rfl::from_generic<T>(value); }

    // JSON
    template<typename T> std::string write_json(const T& obj) { return rfl::json::write(obj); }
    template<typename T> auto read_json(const std::string& json) { return rfl::json::read<T>(json); }

    // TOML
    template<typename T> std::string write_toml(const T& obj) { return rfl::toml::write(obj); }
    template<typename T> auto read_toml(const std::string& toml) { return rfl::toml::read<T>(toml); }

    // MsgPack
    template<typename T> std::vector<char> write_msgpack(const T& obj) { return rfl::msgpack::write(obj); }
    template<typename T> auto read_msgpack(const std::vector<char>& data) { return rfl::msgpack::read<T>(data); }

#ifdef ELYSIA_ENABLE_CAPNPROTO
    // Cap'n Proto
    template<typename T> std::vector<char> write_capnp(const T& obj) { return rfl::capnp::write(obj); }
    template<typename T> auto read_capnp(const std::vector<char>& data) { return rfl::capnp::read<T>(data); }
    template<typename T> std::string get_capnp_schema() { return rfl::capnp::to_schema<T>().str(); }
#endif
}
