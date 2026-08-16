module;
#include <variant>
#include <string_view>
#include <cassert>
#include <cstdint>

export module elysia.result;

export namespace elysia {

enum class ErrorCode : uint32_t {
    Success = 0, NotFound = 404, AlreadyExists = 409, InvalidOperation = 400, InternalError = 500, OutOfMemory = 507, OutOfBounds = 403
};

template<typename T>
class ELYSIA_API Result {
public:
    struct ErrorInfo { ErrorCode code; std::string_view message; };
    static Result ok(T&& v) { return Result(std::move(v)); }
    static Result ok(const T& v) { return Result(v); }
    static Result err(ErrorCode c, std::string_view m = "") { return Result(ErrorInfo{c, m}); }
    bool is_ok() const noexcept { return std::holds_alternative<T>(data_); }
    bool is_err() const noexcept { return !is_ok(); }
    T& unwrap() { assert(is_ok()); return std::get<T>(data_); }
    const ErrorInfo& error() const { assert(is_err()); return std::get<ErrorInfo>(data_); }
private:
    explicit Result(T&& v) : data_(std::move(v)) {}
    explicit Result(const T& v) : data_(v) {}
    explicit Result(ErrorInfo e) : data_(e) {}
    std::variant<T, ErrorInfo> data_;
};

template<>
class ELYSIA_API Result<void> {
public:
    struct ErrorInfo { ErrorCode code; std::string_view message; };
    static Result ok() { return Result(true); }
    static Result err(ErrorCode c, std::string_view m = "") { return Result(ErrorInfo{c, m}); }
    inline bool is_ok() const noexcept { return is_success_; }
    inline bool is_err() const noexcept { return !is_success_; }
    inline void unwrap() const { assert(is_success_); }
    inline const ErrorInfo& error() const { assert(is_err()); return error_; }
    
    inline explicit operator bool() const noexcept { return is_ok(); }
    inline bool operator!() const noexcept { return is_err(); }
private:
    explicit Result(bool s) : is_success_(s) {}
    explicit Result(ErrorInfo e) : is_success_(false), error_(e) {}
    bool is_success_;
    ErrorInfo error_;
};

} // namespace elysia