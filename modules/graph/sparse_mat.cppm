export module graph.impl:spmat;
import std;
import :basis;
#ifndef NO_UNIQUE_ADDRESS_H
#define NO_UNIQUE_ADDRESS_H

#if defined(_MSC_VER)
    #define NO_UNIQUE_ADDRESS_ATTR [[msvc::no_unique_address]]
#elif defined(__GNUC__) || defined(__clang__)
    #define NO_UNIQUE_ADDRESS_ATTR [[no_unique_address]]
#else
    #define NO_UNIQUE_ADDRESS_ATTR
#endif

#endif // NO_UNIQUE_ADDRESS_H

export namespace graph {

// -----------------------------------------------------------------------------
// 2) CSR (Compressed Sparse Row) - 行压缩
// -----------------------------------------------------------------------------
template <class EdgeW> struct CSR {
  std::vector<id_type> indptr;
  std::vector<id_type> indices;

  NO_UNIQUE_ADDRESS_ATTR
  std::conditional_t<Weighted<EdgeW>, std::vector<EdgeW>, WeightBox<void>> data;

  id_type num_rows = 0;
  id_type num_cols = 0;

  id_type rows() const { return num_rows; }
  id_type cols() const { return num_cols; }
  id_type nnz() const { return indices.size(); }

  std::span<const id_type> row_indices(id_type i) const {
    if (i >= num_rows) throw std::out_of_range("CSR row out of range");
    return std::span<const id_type>(indices).subspan(indptr[i], indptr[i + 1] - indptr[i]);
  }

  auto row_weights(id_type i) const {
    if (i >= num_rows) throw std::out_of_range("CSR row out of range");
    if constexpr (Weighted<EdgeW>) {
      return std::span<const EdgeW>(data).subspan(indptr[i], indptr[i + 1] - indptr[i]);
    } else {
      return std::span<const int>{};
    }
  }
};

// -----------------------------------------------------------------------------
// CSC (Compressed Sparse Column) - 列压缩
// -----------------------------------------------------------------------------
template <class EdgeW> struct CSC {
  std::vector<id_type> indptr;
  std::vector<id_type> indices;

  NO_UNIQUE_ADDRESS_ATTR
  std::conditional_t<Weighted<EdgeW>, std::vector<EdgeW>, WeightBox<void>> data;

  id_type num_rows = 0;
  id_type num_cols = 0;

  id_type rows() const { return num_rows; }
  id_type cols() const { return num_cols; }
  id_type nnz() const { return indices.size(); }

  std::span<const id_type> col_indices(id_type j) const {
    if (j >= num_cols) throw std::out_of_range("CSC column out of range");
    return std::span<const id_type>(indices).subspan(indptr[j], indptr[j + 1] - indptr[j]);
  }
};

} // namespace graph
