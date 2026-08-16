module;
#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdint>
export module elysia.archive:utils;

import :model;

 namespace elysia::archive::detail {
     inline static constexpr char B64_CHARS[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

export namespace elysia::archive {

inline std::string base64_encode(const std::string& input) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c; valb += 8;
        while (valb >= 0) { out.push_back(detail::B64_CHARS[(val >> valb) & 0x3F]); valb -= 6; }
    }
    if (valb > -6) out.push_back(detail::B64_CHARS[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

inline std::string base64_decode(const std::string& input) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[detail::B64_CHARS[i]] = i;
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c]; valb += 6;
        if (valb >= 0) { out.push_back(char((val >> valb) & 0xFF)); valb -= 8; }
    }
    return out;
}

// Helper for Raw Buffer I/O
template<typename T> void buf_write(std::vector<char>& buf, const T& val) {
    const char* ptr = reinterpret_cast<const char*>(&val);
    buf.insert(buf.end(), ptr, ptr + sizeof(T));
}
template<typename T> T buf_read(const char*& ptr) {
    T val;
    std::memcpy(&val, ptr, sizeof(T));
    ptr += sizeof(T);
    return val;
}
inline void buf_write_str(std::vector<char>& buf, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    buf_write(buf, len);
    buf.insert(buf.end(), s.begin(), s.end());
}
inline std::string buf_read_str(const char*& ptr) {
    uint32_t len = buf_read<uint32_t>(ptr);
    std::string s(ptr, len);
    ptr += len;
    return s;
}

} // namespace elysia::archive