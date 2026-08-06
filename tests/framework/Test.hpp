#pragma once

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

///
/// A ~150 line test framework.
///
/// Why not GoogleTest or Catch2: both would be fetched at configure time, which
/// means the build stops working on a machine without network - including, very
/// possibly, the Pi sitting on the bench.  The features this project actually
/// uses are "register a test, check a condition, report failures", and owning
/// that is cheaper than owning a dependency.  If the suite ever outgrows it,
/// swapping in Catch2 is a mechanical change to these macros.
///
namespace awtest {

using TestFunction = void (*)();

struct TestCase {
    const char* suite;
    const char* name;
    TestFunction function;
};

/// Collects tests at static initialisation time and runs them.
class Registry {
public:
    static Registry& instance();

    void add(const TestCase& test);

    /// Runs every registered test.  Returns the number of failures.
    int runAll();

    // --- Called by the check macros -----------------------------------------
    void recordFailure(const char* file, int line, const std::string& message);
    [[nodiscard]] bool currentTestFailed() const noexcept { return currentFailed_; }

private:
    std::vector<TestCase> tests_;
    bool currentFailed_{false};
    std::size_t failureCount_{0};
};

/// Registers a test as a side effect of construction.
struct Registrar {
    Registrar(const char* suite, const char* name, TestFunction function) {
        Registry::instance().add(TestCase{suite, name, function});
    }
};

/// Thrown by AW_REQUIRE to abandon the current test.
struct AbortTest {};

[[nodiscard]] std::string describe(bool value);
[[nodiscard]] std::string describe(long long value);
[[nodiscard]] std::string describe(unsigned long long value);
[[nodiscard]] std::string describe(double value);
[[nodiscard]] std::string describe(const std::string& value);
[[nodiscard]] std::string describe(const char* value);

template <typename T>
std::string describeValue(const T& value) {
    if constexpr (std::is_convertible_v<T, std::string>) {
        return describe(std::string{value});
    } else if constexpr (std::is_floating_point_v<T>) {
        return describe(static_cast<double>(value));
    } else if constexpr (std::is_same_v<T, bool>) {
        return describe(value);
    } else if constexpr (std::is_unsigned_v<T>) {
        return describe(static_cast<unsigned long long>(value));
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        return describe(static_cast<long long>(value));
    } else {
        return "<value>";
    }
}

}  // namespace awtest

/// Defines and registers a test.  Usage:
///     AW_TEST(RingBuffer, PushThenPopReturnsSameValue) { ... }
#define AW_TEST(suite, name)                                                    \
    static void aw_test_##suite##_##name();                                     \
    static ::awtest::Registrar aw_registrar_##suite##_##name{#suite, #name,      \
                                                             &aw_test_##suite##_##name}; \
    static void aw_test_##suite##_##name()

/// Records a failure and keeps going.
#define AW_CHECK(condition)                                                     \
    do {                                                                        \
        if (!(condition)) {                                                     \
            ::awtest::Registry::instance().recordFailure(__FILE__, __LINE__,     \
                                                         "expected: " #condition); \
        }                                                                       \
    } while (false)

/// Records a failure and abandons the test.
#define AW_REQUIRE(condition)                                                   \
    do {                                                                        \
        if (!(condition)) {                                                     \
            ::awtest::Registry::instance().recordFailure(__FILE__, __LINE__,     \
                                                         "required: " #condition); \
            throw ::awtest::AbortTest{};                                        \
        }                                                                       \
    } while (false)

#define AW_CHECK_EQ(actual, expected)                                           \
    do {                                                                        \
        const auto aw_actual = (actual);                                        \
        const auto aw_expected = (expected);                                    \
        if (!(aw_actual == aw_expected)) {                                       \
            ::awtest::Registry::instance().recordFailure(                        \
                __FILE__, __LINE__,                                             \
                std::string{#actual} + " == " + #expected + " (actual: " +      \
                    ::awtest::describeValue(aw_actual) + ", expected: " +       \
                    ::awtest::describeValue(aw_expected) + ")");                \
        }                                                                       \
    } while (false)
