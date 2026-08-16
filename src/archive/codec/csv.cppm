module;
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>
export module elysia.archive:codec.csv;

import :model;
import :registry;
import elysia.reflect_wrapper;
import elysia.storage;
import elysia.world;
import elysia.entity;
import elysia.result;

export namespace elysia::archive {

namespace detail {
    inline std::string escape_csv(std::string val) {
        if (val.find(',') != std::string::npos || val.find('"') != std::string::npos || val.find('\n') != std::string::npos) {
            std::string res = "\"";
            for (char c : val) { if (c == '"') res += "\"\""; else res += c; }
            res += "\""; return res;
        }
        return val;
    }

    inline std::vector<std::string> split_csv_line(const std::string& line) {
        std::vector<std::string> cells; std::string cell; bool in_quotes = false;
        for (size_t i = 0; i < line.length(); ++i) {
            char c = line[i];
            if (c == '"') {
                if (in_quotes && i + 1 < line.length() && line[i+1] == '"') { cell += '"'; i++; }
                else in_quotes = !in_quotes;
            } else if (c == ',' && !in_quotes) { cells.push_back(cell); cell.clear(); }
            else if (c != '\r' && c != '\n') cell += c;
        }
        cells.push_back(cell); return cells;
    }

    inline void flatten_top_level(const std::string& prefix, const reflect::Generic& g, std::map<std::string, std::string>& out) {
        auto json = reflect::write_json(g);
        auto map_res = reflect::read_json<std::map<std::string, reflect::Generic>>(json);
        if (map_res) {
            for (const auto& [k, v] : *map_res) {
                std::string v_str = reflect::write_json(v);
                if (v_str.size() >= 2 && v_str.front() == '"' && v_str.back() == '"') v_str = v_str.substr(1, v_str.size() - 2);
                out[prefix + "." + k] = v_str;
            }
        } else {
            std::string v_str = reflect::write_json(g);
            if (v_str.size() >= 2 && v_str.front() == '"' && v_str.back() == '"') v_str = v_str.substr(1, v_str.size() - 2);
            out[prefix] = v_str;
        }
    }
}

inline std::string export_archetype_to_csv(Archetype<>& arch, const std::vector<const ComponentFactory*>& active) {
    std::stringstream csv;
    std::vector<std::map<std::string, std::string>> rows;
    std::vector<std::string> headers = {"id"};
    std::map<std::string, bool> header_map; header_map["id"] = true;

    for (auto& chunk : arch.table().chunks()) {
        for (size_t i = 0; i < chunk->count(); ++i) {
            std::map<std::string, std::string> row; row["id"] = std::to_string(chunk->entity(i).id());
            for (auto* fac : active) {
                if (fac->generic) {
                    auto col_idx = arch.get_column_index(fac->type_id);
                    if (col_idx) {
                        reflect::Generic g = fac->generic->to_generic(chunk->component(*col_idx, i));
                        detail::flatten_top_level(fac->key, g, row);
                    }
                }
            }
            for (const auto& [k, v] : row) { if (!header_map[k]) { header_map[k] = true; headers.push_back(k); } } 
            rows.push_back(std::move(row));
        }
    }
    for (size_t i = 0; i < headers.size(); ++i) csv << headers[i] << (i == headers.size() - 1 ? "" : ",");
    csv << "\n";
    for (const auto& row : rows) {
        for (size_t i = 0; i < headers.size(); ++i) {
            auto it = row.find(headers[i]);
            csv << detail::escape_csv(it != row.end() ? it->second : "") << (i == headers.size() - 1 ? "" : ",");
        }
        csv << "\n";
    }
    return csv.str();
}

// --- Standard Loader ---
inline Result<void> load_from_csv_flattened(World& world, CommandBuffer& cmd, const SnapshotRegistry& reg, 
                                  const std::string& csv, const std::unordered_map<uint64_t, Entity>& id_map) {
    std::stringstream ss(csv); std::string line; if (!std::getline(ss, line)) return Result<void>::ok();
    auto headers = detail::split_csv_line(line);
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        auto cells = detail::split_csv_line(line); if (cells.size() < 2) continue;
        uint64_t raw_id = std::stoull(cells[0]); if (!id_map.contains(raw_id)) continue;
        Entity target = id_map.at(raw_id);
        std::map<std::string, std::map<std::string, std::string>> to_build; 
        for (size_t i = 1; i < cells.size(); ++i) {
            if (i >= headers.size()) break;
            size_t dot = headers[i].find('.'); if (dot == std::string::npos) continue;
            to_build[headers[i].substr(0, dot)][headers[i].substr(dot + 1)] = cells[i];
        }
        for (const auto& [comp, fields] : to_build) {
            std::string json = "{"; bool first = true;
            for (const auto& [fk, fv] : fields) {
                if (!first) json += ",";
                // Re-quote string values: flatten_top_level strips JSON quotes so
                // bare strings must be re-quoted here.  Numbers, booleans, null,
                // and nested objects/arrays are valid JSON primitives and pass through.
                bool is_primitive = false;
                if (fv == "true" || fv == "false" || fv == "null") {
                    is_primitive = true;
                } else if (!fv.empty() && (fv.front() == '{' || fv.front() == '[')) {
                    is_primitive = true;
                } else if (!fv.empty()) {
                    char* end = nullptr;
                    std::strtod(fv.c_str(), &end);
                    is_primitive = (end && end != fv.c_str() && *end == '\0');
                }
                json += "\""; json += fk; json += "\":";
                if (is_primitive) {
                    json += fv;
                } else {
                    json += "\"";
                    for (char c : fv) {
                        if (c == '"')  { json += "\\\""; }
                        else if (c == '\\') { json += "\\\\"; }
                        else { json += c; }
                    }
                    json += "\"";
                }
                first = false;
            }
            json += "}";
            for (const auto& [fid, fac] : reg.factories()) {
                if (fac.key == comp) {
                    if (fac.generic) {
                        auto g = reflect::read_json<reflect::Generic>(json);
                        if (g) {
                            auto res = fac.generic->from_generic_cmd(cmd, target, *g);
                            if (res.is_err()) return res;
                        } else {
                             return Result<void>::err(ErrorCode::InternalError, "Failed to parse CSV JSON");
                        }
                    }
                    break;
                }
            }
        }
    }
    return Result<void>::ok();
}

// --- Remap Loader ---
inline Result<void> load_from_csv_flattened(World& world, CommandBuffer& cmd, const SnapshotRegistry& reg, 
                                  const std::string& csv,
                                  const IDRemapRegistry& id_reg, const IDMapper& mapper) {
    std::stringstream ss(csv); std::string line; if (!std::getline(ss, line)) return Result<void>::ok();
    auto headers = detail::split_csv_line(line);
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        auto cells = detail::split_csv_line(line); if (cells.size() < 2) continue;
        uint64_t raw_id = std::stoull(cells[0]);
        // 🌸 Direct Mapping
        Entity target = mapper.map(raw_id);
        
        std::map<std::string, std::map<std::string, std::string>> to_build; 
        for (size_t i = 1; i < cells.size(); ++i) {
            if (i >= headers.size()) break;
            size_t dot = headers[i].find('.'); if (dot == std::string::npos) continue;
            to_build[headers[i].substr(0, dot)][headers[i].substr(dot + 1)] = cells[i];
        }
        for (const auto& [comp, fields] : to_build) {
            std::string json = "{"; bool first = true;
            for (const auto& [fk, fv] : fields) {
                if (!first) json += ",";
                bool is_primitive = false;
                if (fv == "true" || fv == "false" || fv == "null") {
                    is_primitive = true;
                } else if (!fv.empty() && (fv.front() == '{' || fv.front() == '[')) {
                    is_primitive = true;
                } else if (!fv.empty()) {
                    char* end = nullptr;
                    std::strtod(fv.c_str(), &end);
                    is_primitive = (end && end != fv.c_str() && *end == '\0');
                }
                json += "\""; json += fk; json += "\":";
                if (is_primitive) {
                    json += fv;
                } else {
                    json += "\"";
                    for (char c : fv) {
                        if (c == '"')  { json += "\\\""; }
                        else if (c == '\\') { json += "\\\\"; }
                        else { json += c; }
                    }
                    json += "\"";
                }
                first = false;
            }
            json += "}";
            for (const auto& [fid, fac] : reg.factories()) {
                if (fac.key == comp) {
                    if (fac.generic) {
                        auto g = reflect::read_json<reflect::Generic>(json);
                        if (g) {
                            auto res = fac.generic->from_generic_cmd(cmd, target, *g);
                            if (res.is_err()) return res;

                            // 🌸 Remap Surgery
                            if (const auto* hook = id_reg.get_hook(fac.type_id)) {
                                void* ptr = cmd.last_payload();
                                if (ptr) (*hook)(ptr, mapper);
                            }
                        } else {
                             return Result<void>::err(ErrorCode::InternalError, "Failed to parse CSV JSON");
                        }
                    }
                    break;
                }
            }
        }
    }
    return Result<void>::ok();
}

} // namespace elysia::archive