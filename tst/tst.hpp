#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tst {
struct TestRegistrar;

// ANSI color codes
inline constexpr auto ANSI_RED = "\x1B[31m";
inline constexpr auto ANSI_GREEN = "\x1B[32m";
inline constexpr auto ANSI_RESET = "\033[0m";

// Concept for types which can be printed with std::format
// Mostly equivalent to C++23 std::formattable
// For simplicity only compatible with char, not wchar
template <typename T, typename Context,
          typename Formatter = Context::template formatter_type<std::remove_const_t<T>>>
concept FormattableWith =
    std::semiregular<Formatter> &&
    requires(Formatter& f, const Formatter& cf, T&& t, Context fc,
             std::basic_format_parse_context<typename Context::char_type> pc) {
        { f.parse(pc) } -> std::same_as<typename decltype(pc)::iterator>;
        { cf.format(t, fc) } -> std::same_as<typename Context::iterator>;
    };

template <typename T>
concept Formattable = FormattableWith<std::remove_reference_t<T>, std::format_context>;

// Types wrapped in this wrapper can be printed with our custom formatter as a byte-sequence, if
// they don't already have a formatter
template <typename T>
struct Printer {
    const T& value;
};

// Type that, when printed, automatically adds "s" or "S" to the label if count is not "1"
struct Plural {
    std::size_t count;
    std::string_view label;
};

// Type that, when printed, expands to the shown string and color
enum class Status {
    TestingSep,  // [==========] (Green)
    SuiteSep,    // [----------] (Green)
    TestRun,     // [ RUN      ] (Green)
    TestOk,      // [       OK ] (Green)
    TestFail,    // [  FAILED  ] (Red)
    TestSkip,    // [  SKIPPED ] (Green)
    NrPassed,    // [  PASSED  ] (Green)
};

}  // namespace tst

// Print singular or plural depending on count
template <>
struct std::formatter<tst::Plural> : std::formatter<std::size_t> {
    auto format(const tst::Plural& p, format_context& ctx) const {
        // Determine the suffix based on the last character of the label
        const bool isUpper = !p.label.empty() && std::isupper(p.label.back());

        const std::string_view suffix = (p.count == 1) ? "" : (isUpper ? "S" : "s");

        // Forward the number formatting to the base size_t formatter
        return std::format_to(std::formatter<std::size_t>::format(p.count, ctx), " {}{}", p.label,
                              suffix);
    }
};

// Print colored status indicator
template <>
struct std::formatter<tst::Status> {
    // No flags
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    auto format(tst::Status s, format_context& ctx) const {
        std::string_view label;
        std::string_view color;
        using enum tst::Status;

        switch (s) {
            case TestingSep: label = "[==========]"; break;
            case SuiteSep: label = "[----------]"; break;
            case TestRun: label = "[ RUN      ]"; break;
            case TestOk: label = "[       OK ]"; break;
            case TestFail: label = "[  FAILED  ]"; break;
            case TestSkip: label = "[  SKIPPED ]"; break;
            case NrPassed: label = "[  PASSED  ]"; break;
        }
        color = (s == TestFail) ? tst::ANSI_RED : tst::ANSI_GREEN;

        return std::format_to(ctx.out(), "{}{}{}", color, label, tst::ANSI_RESET);
    }
};

// Print unknown type wrapped in Printer
template <typename T>
struct std::formatter<tst::Printer<T>> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    auto format(tst::Printer<T> wrapper, format_context& ctx) const {
        const T& t = wrapper.value;

        // If type is already formattable
        if constexpr (tst::Formattable<T>) {
            return std::format_to(ctx.out(), "{}", t);
        }
        // Fallback to hex dump
        else {
            auto out = std::format_to(ctx.out(), "<non-printable type:");

            const auto* bytes = reinterpret_cast<const std::byte*>(&t);
            static constexpr auto LIMIT = std::min(sizeof(T), std::size_t{8});

            for (std::size_t i = 0; i < LIMIT; ++i) {
                out = std::format_to(out, " {:02x}", std::to_integer<int>(bytes[i]));
            }

            if constexpr (sizeof(T) > LIMIT) {
                out = std::format_to(out, "...");
            }

            return std::format_to(out, ">");
        }
    }
};

// Singleton that registers all test functors during static initialization,
// calls them in runAllTests and stores info about the current test function.
struct GlobalTestManager {
    [[nodiscard]] static GlobalTestManager& getInstance() {
        // Thread-safe initialization on first use.
        static GlobalTestManager instance;
        return instance;
    }

    GlobalTestManager(const GlobalTestManager&) = delete;
    GlobalTestManager& operator=(const GlobalTestManager&) = delete;

    // Returns 0 if all tests ran successfully
    int runAllTests() {
        using namespace std::chrono;
        using namespace tst;
        const auto nrTestSuites = static_cast<unsigned>(tests.size());
        const auto nrTests =
            std::transform_reduce(tests.begin(), tests.end(), 0u, std::plus<>(),
                                  [](const decltype(tests)::value_type& v) {
                                      return static_cast<unsigned>(v.second.size());
                                  });
        auto nrSuccessfulTests = 0u;
        std::vector<std::string> skippedTests{};
        std::vector<std::string> failedTests{};

        std::cout << std::format("{} Running {} from {}.\n", Status::TestingSep,
                                 Plural{nrTests, "test"}, Plural{nrTestSuites, "test suite"});

        const auto totalT1 = steady_clock::now();
        for (const auto& [testSuiteName, testSuiteTests] : tests) {
            std::cout << std::format("{} {} from {} \n", Status::SuiteSep,
                                     Plural{testSuiteTests.size(), "test"}, testSuiteName);
            auto suiteT1 = steady_clock::now();
            for (const auto& [testName, testFunction] : testSuiteTests) {
                // Test function wrapper is called here
                testFunction();
                if (activeTestFailures > 0) {
                    failedTests.emplace_back(std::format("{}.{}", testSuiteName, testName));
                } else if (activeTestSkipped) {
                    skippedTests.emplace_back(std::format("{}.{}", testSuiteName, testName));
                } else {
                    ++nrSuccessfulTests;
                }
                activeTestFailures = 0;
                activeTestSkipped = false;
            }
            auto suiteT2 = steady_clock::now();
            std::cout << std::format("{} {} from {} ({} total)\n\n", Status::SuiteSep,
                                     Plural{testSuiteTests.size(), "test"}, testSuiteName,
                                     duration_cast<milliseconds>(suiteT2 - suiteT1));
        }
        const auto totalT2 = steady_clock::now();

        std::cout << std::format("{} {} from {} ran. ({} total)\n", Status::TestingSep,
                                 Plural{nrTests, "test"}, Plural{nrTestSuites, "test suite"},
                                 duration_cast<milliseconds>(totalT2 - totalT1));

        std::cout << std::format("{} {}.\n", Status::NrPassed, Plural{nrSuccessfulTests, "test"});

        if (!skippedTests.empty()) {
            std::cout << std::format("{} {}, listed below:\n", Status::TestSkip,
                                     Plural{skippedTests.size(), "test"});
            for (const auto& skippedTestName : skippedTests) {
                std::cout << std::format("{} {}\n", Status::TestSkip, skippedTestName);
            }
        }

        if (!failedTests.empty()) {
            std::cout << std::format("{} {}, listed below:\n", Status::TestFail,
                                     Plural{failedTests.size(), "test"});
            for (const auto& failedTestName : failedTests) {
                std::cout << std::format("{} {}\n", Status::TestFail, failedTestName);
            }
            std::cout << std::format("\n{:2}\n", Plural{failedTests.size(), "FAILED TEST"});
        }

        const auto encounteredTests = nrSuccessfulTests + skippedTests.size() + failedTests.size();
        if (encounteredTests != nrTests) {
            std::cout << std::format(
                "{}\nDiscrepancy between number of expected and number of run tests!\n{}", ANSI_RED,
                ANSI_RESET);
            std::cout << std::format("\tSuccessful tests:        {}\n", nrSuccessfulTests);
            std::cout << std::format("\tSkipped tests:           {}\n", skippedTests.size());
            std::cout << std::format("\tFailed tests:            {}\n", failedTests.size());
            std::cout << "\t---\n";
            std::cout << std::format("\tTotal encountered tests: {}\n", encounteredTests);
            std::cout << std::format("\tExpected tests:          {}\n\n", nrTests);
        }

        std::cout << std::flush;
        return !failedTests.empty();
    }

    // Global variables that the currently running test writes to during a test
    // and the test manager reads from after a test.
    // Passing these values around via the test function return would make
    // calling subroutines in test functions more difficult.
    int activeTestFailures = 0;
    bool activeTestSkipped = false;

   private:
    void add(const std::string& testSuite, const std::string& test,
             const std::function<void()>& fun) {
        const std::scoped_lock lock(testAppendLock);
        tests[testSuite].emplace_back(test, fun);
    }

    // Maps test suit names to vector testname-testfunction-pairs
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::function<void()>>>>
        tests;

    // During initialization, locks the tests map while objects are appended
    std::mutex testAppendLock;

    // Don't allow new instantiations of singleton
    GlobalTestManager() = default;

    friend struct tst::TestRegistrar;
};

namespace tst {
struct PredicateFailureException : std::exception {};

// Run and time the given test function and print the result.
inline void printRunTest(const char* fullTestName, const std::function<void()>& fun) {
    using namespace std::chrono;
    std::cout << std::format("{} {}", Status::TestRun, fullTestName) << std::endl;
    const auto t1 = steady_clock::now();
    try {
        // This actually calls the test function
        fun();
    } catch (PredicateFailureException&) {
        // The test failure counter should already be incremented, unless the
        // user manually threw a PredicateFailureException
        if (GlobalTestManager::getInstance().activeTestFailures == 0) {
            GlobalTestManager::getInstance().activeTestFailures = 1;
        }
    } catch (std::exception& e) {
        std::cout << std::format("Exception was thrown during test execution: {}\n", e.what());
        ++GlobalTestManager::getInstance().activeTestFailures;
    } catch (...) {
        std::cout << "Unknown exception was thrown during test execution.\n";
        ++GlobalTestManager::getInstance().activeTestFailures;
    }
    const auto t2 = steady_clock::now();
    if (GlobalTestManager::getInstance().activeTestFailures > 0) {
        std::cout << std::format("{}", Status::TestFail);
    } else if (GlobalTestManager::getInstance().activeTestSkipped) {
        std::cout << std::format("{}", Status::TestSkip);
    } else {
        std::cout << std::format("{}", Status::TestOk);
    }
    std::cout << std::format(" {} ({})", fullTestName, duration_cast<milliseconds>(t2 - t1))
              << std::endl;
}

// The TEST macros create global objects of this type so that its constructor
// registers the test function.
struct TestRegistrar {
    TestRegistrar(const char* testSuiteName, const char* testName,
                  const std::function<void()>& fun) {
        GlobalTestManager::getInstance().add(testSuiteName, testName, fun);
    }
};

[[noreturn]] inline void fatalFailure() {
    ++GlobalTestManager::getInstance().activeTestFailures;
    throw PredicateFailureException();
}
inline void nonFatalFailure() { ++GlobalTestManager::getInstance().activeTestFailures; }
using FailureHandler = void (*)();

inline void boolTestFailure(bool expected, const char* condition, FailureHandler f,
                            const char* file, int line) {
    std::cout << std::format("{} ({}): error: Value of: {}\n  Actual: {}\nExpected: {}\n", file,
                             line, condition, !expected, expected)
              << std::endl;
    f();
}
template <typename T1, typename T2>
inline void predicateTestFailure(const T1& actualPredicateValue, const T2& expectedPredicateValue,
                                 const char* actualPredicateText, const char* expectedPredicateText,
                                 const char* predicate, FailureHandler f, const char* file,
                                 int line) {
    std::cout << std::format("{}({}): error: Expected: {} {} {}, actual: {} vs {}\n", file, line,
                             actualPredicateText, predicate, expectedPredicateText,
                             Printer{actualPredicateValue}, Printer{expectedPredicateValue})
              << std::endl;
    f();
}

[[nodiscard]] inline bool isTrue(bool condition) { return condition; }

[[nodiscard]] inline bool alwaysTrue() {
    // This condition is always false so alwaysTrue() never actually throws,
    // but it makes the compiler think that it may throw.
    if (isTrue(false)) {
        throw std::exception();  // Should never be thrown
    }
    return true;
}
}  // namespace tst

// Define a test function
#define TEST(TestSuite, Test)                                                           \
    static void TestSuite##_##Test##TestFun();                                          \
    const tst::TestRegistrar TestSuite##_##Test##_##Registrar(#TestSuite, #Test, []() { \
        tst::printRunTest(#TestSuite "." #Test, TestSuite##_##Test##TestFun);           \
    });                                                                                 \
    static void TestSuite##_##Test##TestFun()

// Define a parametrized test function
#define TEST_P(TestSuite, Test, Parameter) static void TestSuite##_##Test##TestFun(Parameter)

// Instantiate a parametrized test function with a value for the parameter
#define INSTANTIATE_TEST_SUITE_P(TestSuite, Test, ParameterValue)                      \
    const tst::TestRegistrar TestSuite##_##Test##_##Registrar_##ParameterValue(        \
        #TestSuite, #Test "/" #ParameterValue, []() {                                  \
            tst::printRunTest(#TestSuite "." #Test "/" #ParameterValue,                \
                              std::bind(TestSuite##_##Test##TestFun, ParameterValue)); \
        })

#if defined(_MSC_VER) && !defined(__clang__)
#define TST_SUPPRESS_CONSTANT_CONDITIONAL_EXPRESSION_WARNING __pragma(warning(suppress : 4127))
#else
#define TST_SUPPRESS_CONSTANT_CONDITIONAL_EXPRESSION_WARNING
#endif

// Likely / Unlikely
#if defined(__GNUC__)
#define TST_IF_UNLIKELY(expr) if (__builtin_expect(!!(expr), 0))
#elif __has_cpp_attribute(likely)
#define TST_IF_UNLIKELY(expr) if (expr) [[unlikely]]
#else
#define TST_IF_UNLIKELY(expr) if (expr)
#endif

#define TEST_BOOLEAN(condition, expected, FailureHandler)                                     \
    do {                                                                                      \
        TST_SUPPRESS_CONSTANT_CONDITIONAL_EXPRESSION_WARNING TST_IF_UNLIKELY(!!(condition) != \
                                                                             (expected)) {    \
            tst::boolTestFailure(expected, #condition, FailureHandler, __FILE__, __LINE__);   \
        }                                                                                     \
    } while (false)

#define TEST_PREDICATE(condition, expected, predicate, FailureHandler)                        \
    do {                                                                                      \
        TST_IF_UNLIKELY(!((condition)predicate(expected))) {                                  \
            tst::predicateTestFailure(condition, expected, #condition, #expected, #predicate, \
                                      FailureHandler, __FILE__, __LINE__);                    \
        }                                                                                     \
    } while (false)

#define TEST_FAIL(FailureHandler)                                                               \
    do {                                                                                        \
        std::cout << std::format("{}({}): error: Explicit test failure\n", __FILE__, __LINE__); \
        FailureHandler();                                                                       \
    } while (false)

// Explicit failures
#define ADD_FAILURE() TEST_FAIL(tst::nonFatalFailure)
#define FAIL() TEST_FAIL(tst::fatalFailure)

// Verify condition
#define EXPECT_TRUE(condition) TEST_BOOLEAN(condition, true, tst::nonFatalFailure)
#define EXPECT_FALSE(condition) TEST_BOOLEAN(condition, false, tst::nonFatalFailure)
#define ASSERT_TRUE(condition) TEST_BOOLEAN(condition, true, tst::fatalFailure)
#define ASSERT_FALSE(condition) TEST_BOOLEAN(condition, false, tst::fatalFailure)

// Binary comparisons
#define EXPECT_EQ(val1, val2) TEST_PREDICATE(val1, val2, ==, tst::nonFatalFailure)
#define EXPECT_NE(val1, val2) TEST_PREDICATE(val1, val2, !=, tst::nonFatalFailure)
#define EXPECT_LE(val1, val2) TEST_PREDICATE(val1, val2, <=, tst::nonFatalFailure)
#define EXPECT_LT(val1, val2) TEST_PREDICATE(val1, val2, <, tst::nonFatalFailure)
#define EXPECT_GE(val1, val2) TEST_PREDICATE(val1, val2, >=, tst::nonFatalFailure)
#define EXPECT_GT(val1, val2) TEST_PREDICATE(val1, val2, >, tst::nonFatalFailure)

#define ASSERT_EQ(val1, val2) TEST_PREDICATE(val1, val2, ==, tst::fatalFailure)
#define ASSERT_NE(val1, val2) TEST_PREDICATE(val1, val2, !=, tst::fatalFailure)
#define ASSERT_LE(val1, val2) TEST_PREDICATE(val1, val2, <=, tst::fatalFailure)
#define ASSERT_LT(val1, val2) TEST_PREDICATE(val1, val2, <, tst::fatalFailure)
#define ASSERT_GE(val1, val2) TEST_PREDICATE(val1, val2, >=, tst::fatalFailure)
#define ASSERT_GT(val1, val2) TEST_PREDICATE(val1, val2, >, tst::fatalFailure)

#define TEST_THROW(statement, ExceptionType, FailureHandler)                                     \
    do {                                                                                         \
        try {                                                                                    \
            statement;                                                                           \
            std::cout << std::format("{}({}): error: {}\n", __FILE__, __LINE__,                  \
                                     "Statement \"" #statement "\" did not throw an exception"); \
            FailureHandler();                                                                    \
        } catch (ExceptionType) {                                                                \
        } catch (...) {                                                                          \
            std::cout << std::format("{}({}): error: {}\n", __FILE__, __LINE__,                  \
                                     "Statement \"" #statement                                   \
                                     "\" threw an unexpected exception");                        \
            FailureHandler();                                                                    \
        }                                                                                        \
    } while (false)

#define TEST_NO_THROW(statement, FailureHandler)                                         \
    do {                                                                                 \
        try {                                                                            \
            statement;                                                                   \
        } catch (...) {                                                                  \
            std::cout << std::format("{}({}): error: {}\n", __FILE__, __LINE__,          \
                                     "Statement \"" #statement "\" threw an exception"); \
            FailureHandler();                                                            \
        }                                                                                \
    } while (false)

// Exception assertions
#define EXPECT_THROW(statement, ExceptionType) \
    TEST_THROW(statement, const ExceptionType&, tst::nonFatalFailure)
#define EXPECT_ANY_THROW(statement) TEST_THROW(statement, ..., tst::nonFatalFailure)
#define EXPECT_NO_THROW(statement) TEST_NO_THROW(statement, tst::nonFatalFailure)
#define ASSERT_THROW(statement, ExceptionType) \
    TEST_THROW(statement, const ExceptionType&, tst::fatalFailure)
#define ASSERT_ANY_THROW(statement) TEST_THROW(statement, ..., tst::fatalFailure)
#define ASSERT_NO_THROW(statement) TEST_NO_THROW(statement, tst::fatalFailure)

// Skip the current test
#define SKIP_TEST()                                                \
    do {                                                           \
        GlobalTestManager::getInstance().activeTestSkipped = true; \
        return;                                                    \
    } while (false)

// Useful together with SKIP_TEST
#define TST_SUPPRESS_UNREACHABLE_CODE_WARNING_BELOW(statement) \
    do {                                                       \
        if (tst::alwaysTrue()) {                               \
            statement;                                         \
        }                                                      \
    } while (false)
