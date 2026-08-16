module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <tuple>
#include <type_traits>
#include <vector>

export module elysia.bitset;

export namespace elysia {
/**
 * @brief Hybrid Signature Buffer with SBO.
 * Now fully compatible with std::unordered_map.
 */
template <size_t N = 4> struct SignatureBuffer {
  using StorageType = std::conditional_t<(N > 0), std::array<uint64_t, N>,
                                         std::vector<uint64_t>>;

  StorageType data;

  SignatureBuffer() {
    if constexpr (N == 0) {
      data.resize(2, 0);
    } else {
      data.fill(0);
    }
  }

  [[nodiscard]] size_t size() const noexcept { return data.size(); }

  [[nodiscard]] size_t effective_size() const noexcept {
    size_t n = data.size();
    while (n > 0 && data[n - 1] == 0)
      --n;
    return n;
  }

  void set(uint32_t bit_idx) {
    const uint32_t chunk_idx = bit_idx >> 6;
    const uint64_t bit_mask = 1ULL << (bit_idx & 63);

    if constexpr (N == 0) {
      if (chunk_idx >= data.size()) {
        data.resize(chunk_idx + 1, 0);
      }
    }

    data[chunk_idx] |= bit_mask;
  }

  bool try_set(uint32_t bit_idx) {
    const uint32_t chunk_idx = bit_idx >> 6;
    const uint64_t bit_mask = 1ULL << (bit_idx & 63);

    if constexpr (N == 0) {
      if (chunk_idx >= data.size()) {
        data.resize(chunk_idx + 1, 0);
      }
    } else {
      if (chunk_idx >= N)
        return false;
    }

    data[chunk_idx] |= bit_mask;
    return true;
  }

  void reset(uint32_t bit_idx) noexcept {
    const uint32_t chunk_idx = bit_idx >> 6;
    if (chunk_idx >= data.size())
      return;

    const uint64_t bit_mask = 1ULL << (bit_idx & 63);
    data[chunk_idx] &= ~bit_mask;
  }

  [[nodiscard]] bool test(uint32_t bit_idx) const noexcept {
    const uint32_t chunk_idx = bit_idx >> 6;
    if (chunk_idx >= data.size())
      return false;

    return (data[chunk_idx] & (1ULL << (bit_idx & 63))) != 0;
  }

  [[nodiscard]] bool operator==(const SignatureBuffer &other) const noexcept {
    const size_t n0 = effective_size();
    const size_t n1 = other.effective_size();

    if (n0 != n1)
      return false;

    for (size_t i = 0; i < n0; ++i) {
      if (data[i] != other.data[i])
        return false;
    }

    return true;
  }

  [[nodiscard]] size_t hash_value() const noexcept {
    size_t h = 0;
    const size_t n = effective_size();

    for (size_t i = 0; i < n; ++i) {
      const uint64_t val = data[i];
      h ^= static_cast<size_t>(val) +
           static_cast<size_t>(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
    }

    return h;
  }
};
/**
 * @brief ADL-friendly match functions.
 */
template <size_t N>
[[nodiscard]] inline bool match_all(const SignatureBuffer<N> &arch,
                                    const SignatureBuffer<N> &with) noexcept {
  const size_t check_len = std::min(arch.data.size(), with.data.size());

  for (size_t i = 0; i < check_len; ++i) {
    if ((arch.data[i] & with.data[i]) != with.data[i]) {
      return false;
    }
  }

  for (size_t i = check_len; i < with.data.size(); ++i) {
    if (with.data[i] != 0)
      return false;
  }

  return true;
}

template <size_t N>
[[nodiscard]] inline bool intersects(const SignatureBuffer<N> &a,
                                     const SignatureBuffer<N> &b) noexcept {
  const size_t check_len = std::min(a.data.size(), b.data.size());

  for (size_t i = 0; i < check_len; ++i) {
    if ((a.data[i] & b.data[i]) != 0)
      return true;
  }

  return false;
}

} // namespace elysia

namespace std {
template <size_t N> struct hash<elysia::SignatureBuffer<N>> {
  size_t operator()(const elysia::SignatureBuffer<N> &sig) const noexcept {
    return sig.hash_value();
  }
};
} // namespace std