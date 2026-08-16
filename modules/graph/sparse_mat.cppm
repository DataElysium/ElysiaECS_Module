
export module graph.impl:spmat;
import std;
import :basis;
#ifndef NO_UNIQUE_ADDRESS_H
#define NO_UNIQUE_ADDRESS_H

// 检查是否为 MSVC 编译器 (通常用于 Windows)
#if defined(_MSC_VER)  
    // MSVC 19.29 (VS 2019 16.10) 或更高版本支持 C++20 [[no_unique_address]] 
    // 但为了确保兼容性，使用 MSVC 特定的形式
    #define NO_UNIQUE_ADDRESS_ATTR [[msvc::no_unique_address]]

// 检查是否为 GCC 或 Clang 编译器 (通常用于 Linux/macOS)
// GCC 9+ 或 Clang 11+ 才支持标准 [[no_unique_address]]
#elif defined(__GNUC__) || defined(__clang__)
    // 使用 C++ 标准属性
    #define NO_UNIQUE_ADDRESS_ATTR [[no_unique_address]]

// 如果不支持，则宏为空，相当于禁用此优化
#else
    #define NO_UNIQUE_ADDRESS_ATTR
#endif

#endif // NO_UNIQUE_ADDRESS_H

export namespace graph {

// -----------------------------------------------------------------------------
// 2) CSR (Compressed Sparse Row) - 行压缩
// -----------------------------------------------------------------------------
template <class EdgeW> struct CSR {
  std::vector<id_type> indptr;  // 行偏移 (Row Pointers)
  std::vector<id_type> indices; // 列索引 (Column Indices)

  // 魔法数据成员：无权重时消失
  NO_UNIQUE_ADDRESS_H
  std::conditional_t<Weighted<EdgeW>, std::vector<EdgeW>, WeightBox<void>> data;

  id_type num_rows;
  id_type num_cols;

  // 语义 API：防止你把 CSR 当 CSC 用
  id_type rows() const { return num_rows; }
  id_type cols() const { return num_cols; }
  id_type nnz() const { return indices.size(); }

  // 获取第 i 行的数据（切片）
  std::span<const id_type> row_indices(id_type i) const {
    return {&indices[indptr[i]], &indices[indptr[i + 1]]};
  }

  // 获取权重切片（如果有）
  auto row_weights(id_type i) const {
    if constexpr (Weighted<EdgeW>) {
      return std::span<const EdgeW>{&data[indptr[i]], &data[indptr[i + 1]]};
    } else {
      return std::span<const int>{}; // 空切片
    }
  }
};

// -----------------------------------------------------------------------------
//  CSC (Compressed Sparse Column) - 列压缩
// -----------------------------------------------------------------------------
template <class EdgeW> struct CSC {
  std::vector<id_type>
      indptr; // 列偏移 (Column Pointers) !!! 名字虽然叫indptr，但语义变了
  std::vector<id_type> indices; // 行索引 (Row Indices)

  NO_UNIQUE_ADDRESS_H
  std::conditional_t<Weighted<EdgeW>, std::vector<EdgeW>, WeightBox<void>> data;

  id_type num_rows;
  id_type num_cols;

  // 语义 API
  id_type rows() const { return num_rows; }
  id_type cols() const { return num_cols; }
  id_type nnz() const { return indices.size(); }

  // 获取第 j 列的数据
  std::span<const id_type> col_indices(id_type j) const {
    return {&indices[indptr[j]], &indices[indptr[j + 1]]};
  }

  // CSC 绝对不能有 row_indices() 方法，编译器会打手心哦！
};
} // namespace graph