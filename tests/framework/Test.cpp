#include "framework/Test.hpp"

#include <cstdio>
#include <string>

namespace awtest {

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::add(const TestCase& test) { tests_.push_back(test); }

void Registry::recordFailure(const char* file, int line, const std::string& message) {
    currentFailed_ = true;
    ++failureCount_;
    std::printf("    FAIL %s:%d\n         %s\n", file, line, message.c_str());
}

int Registry::runAll() {
    std::size_t failedTests = 0;

    for (const TestCase& test : tests_) {
        currentFailed_ = false;
        std::printf("  %s.%s\n", test.suite, test.name);

        try {
            test.function();
        } catch (const AbortTest&) {
            // Already reported by AW_REQUIRE.
        } catch (const std::exception& error) {
            recordFailure(__FILE__, __LINE__,
                          std::string{"unexpected exception: "} + error.what());
        } catch (...) {
            recordFailure(__FILE__, __LINE__, "unexpected non-standard exception");
        }

        if (currentFailed_) {
            ++failedTests;
        }
    }

    std::printf("\n%zu test(s), %zu failed, %zu check failure(s)\n", tests_.size(), failedTests,
                failureCount_);
    return static_cast<int>(failedTests);
}

std::string describe(bool value) { return value ? "true" : "false"; }
std::string describe(long long value) { return std::to_string(value); }
std::string describe(unsigned long long value) { return std::to_string(value); }
std::string describe(double value) { return std::to_string(value); }
std::string describe(const std::string& value) { return "\"" + value + "\""; }
std::string describe(const char* value) { return describe(std::string{value}); }

}  // namespace awtest

int main() {
    std::printf("audio-workstation test suite\n\n");
    return awtest::Registry::instance().runAll() == 0 ? 0 : 1;
}
