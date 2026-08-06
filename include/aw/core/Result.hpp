#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "aw/core/Error.hpp"

namespace aw::core {

/// Either a value or an `Error`.
///
/// This is a hand rolled `std::expected`.  `std::expected` is C++23 and the
/// project targets C++20 on two toolchains (GCC on the Pi, the ESP-IDF
/// toolchain later), so owning ~60 lines is cheaper than a compiler upgrade
/// constraint.  The API is intentionally a subset of `std::expected` so the
/// alias can be swapped for the standard type once both toolchains support it.
///
/// Rules of use:
///  - `[[nodiscard]]`: ignoring a failure is a compile warning.
///  - Never constructed on the realtime audio thread (the message allocates).
///    Realtime code reports failure through atomics and the event queue instead.
template <typename T>
class [[nodiscard]] Result {
public:
    using ValueType = T;

    Result(T value) : storage_{std::move(value)} {}          // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_{std::move(error)} {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    /// Precondition: `hasValue()`.  Throws std::bad_variant_access otherwise,
    /// which is a programmer error, not a runtime condition.
    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] T valueOr(T fallback) const& {
        return hasValue() ? std::get<T>(storage_) : std::move(fallback);
    }

    /// Precondition: `!hasValue()`.
    [[nodiscard]] const Error& error() const& { return std::get<Error>(storage_); }

private:
    std::variant<T, Error> storage_;
};

/// `Result<void>` - an operation that either succeeded or failed.
template <>
class [[nodiscard]] Result<void> {
public:
    using ValueType = void;

    Result() = default;
    Result(Error error) : error_{std::move(error)} {}        // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return hasValue(); }

    /// Precondition: `!hasValue()`.
    [[nodiscard]] const Error& error() const& { return *error_; }

private:
    std::optional<Error> error_;
};

/// Reads better than `Result<void>` at call sites that return nothing useful.
using Status = Result<void>;

/// Explicit success, for symmetry with `return makeError(...)`.
[[nodiscard]] inline Status success() noexcept { return Status{}; }

}  // namespace aw::core
