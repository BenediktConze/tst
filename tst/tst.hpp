#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tst {
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

// Concept for types which can be printed with an ostream and operator<<
template <typename T>
concept Streamable = requires(std::ostream& os, T t) {
    { os << t } -> std::convertible_to<std::ostream&>;
};

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

// Busywork C++ makes us do to enable transparent lookup in hash maps
struct TransparentStringHash {
    using HashType = std::hash<std::string_view>;

    using is_transparent = void;

    auto operator()(const char* str) const { return HashType{}(str); }
    auto operator()(std::string_view str) const { return HashType{}(str); }
    auto operator()(const std::string& str) const { return HashType{}(str); }
};

struct PredicateFailureException : std::exception {};

struct SkipTestException : std::exception {};

}  // namespace tst

// Print singular or plural depending on count
template <>
struct std::formatter<tst::Plural> : std::formatter<std::size_t> {
    auto format(const tst::Plural& p, format_context& ctx) const {
        // Determine the suffix based on the last character of the label
        const bool isUpper =
            !p.label.empty() && std::isupper(static_cast<unsigned char>(p.label.back()));

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

        if constexpr (tst::Formattable<T>) {
            // If type is already formattable
            return std::format_to(ctx.out(), "{}", t);
        } else if constexpr (tst::Streamable<T>) {
            // If type is printable with operator<<
            std::ostringstream ss;
            ss << t;
            return std::format_to(ctx.out(), "{}", ss.str());
        } else {
            // Fallback to hex dump
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
        nrSuccessfulTests = 0u;
        skippedTests = {};
        failedTests = {};

        std::cout << std::format("{} Running {} from {}.\n", Status::TestingSep,
                                 Plural{nrTests, "test"}, Plural{nrTestSuites, "test suite"});

        const auto totalT1 = steady_clock::now();
        for (const auto& [testSuiteName, testSuiteTests] : tests) {
            std::cout << std::format("{} {} from {} \n", Status::SuiteSep,
                                     Plural{testSuiteTests.size(), "test"}, testSuiteName);
            auto suiteT1 = steady_clock::now();
            for (const auto& [testName, testFunction] : testSuiteTests) {
                // Test function wrapper is called here
                runTestWrapped(testSuiteName, testName, testFunction);
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

    // Global variable that the currently running test conditionally increments during a test and
    // the test manager reads from after a test.
    // Passing this value around via the test function return would make calling subroutines in test
    // functions more difficult.
    int activeTestFailures = 0;

    void add(std::string_view testSuite, std::string test, std::function<void()> fun) {
        const std::scoped_lock lock(testAppendLock);

        // Using find for transparent lookup
        auto it = tests.find(testSuite);

        // Create new test suite if necessary
        if (it == tests.end()) {
            it = tests.try_emplace(std::string(testSuite)).first;
        }

        // Add test to list of test suite test functions
        it->second.emplace_back(std::move(test), std::move(fun));
    }

   private:
    // Call the actual test function, catch all potential errors, time the whole thing and print all
    // the info
    void runTestWrapped(std::string_view suiteName, std::string_view testName,
                        const std::function<void()>& fun) {
        using namespace std::chrono;
        using namespace tst;
        std::cout << std::format("{} {}.{}", Status::TestRun, suiteName, testName) << std::endl;
        const auto t1 = steady_clock::now();
        bool activeTestSkipped = false;

        try {
            // This actually calls the test function
            fun();
        } catch (PredicateFailureException&) {
            // The test failure counter should already be incremented, unless the
            // user manually threw a PredicateFailureException
            if (activeTestFailures == 0) {
                activeTestFailures = 1;
            }
        } catch (SkipTestException&) {
            activeTestSkipped = true;
        } catch (std::exception& e) {
            std::cout << std::format("Exception was thrown during test execution: {}\n", e.what());
            ++activeTestFailures;
        } catch (...) {
            std::cout << "Unknown exception was thrown during test execution.\n";
            ++activeTestFailures;
        }
        const auto t2 = steady_clock::now();

        if (activeTestFailures > 0) {
            std::cout << std::format("{}", Status::TestFail);
            failedTests.emplace_back(std::format("{}.{}", suiteName, testName));
        } else if (activeTestSkipped) {
            std::cout << std::format("{}", Status::TestSkip);
            skippedTests.emplace_back(std::format("{}.{}", suiteName, testName));
        } else {
            std::cout << std::format("{}", Status::TestOk);
            ++nrSuccessfulTests;
        }
        std::cout << std::format(" {}.{} ({})", suiteName, testName,
                                 duration_cast<milliseconds>(t2 - t1))
                  << std::endl;

        activeTestFailures = 0;
    }

    // Maps test suit names to vector testname-testfunction-pairs
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::function<void()>>>,
                       tst::TransparentStringHash, std::equal_to<>>
        tests;

    // During initialization, locks the tests map while objects are appended
    std::mutex testAppendLock;

    // Keep track of test results during runAllTests
    unsigned nrSuccessfulTests = 0u;
    std::vector<std::string> skippedTests{};
    std::vector<std::string> failedTests{};

    // Don't allow new instantiations of singleton
    GlobalTestManager() = default;
};

namespace tst {
// The TEST macros create global objects of this type so that its constructor
// registers the test function.
struct TestRegistrar {
    // Registers the test function fun under the given names
    TestRegistrar(std::string_view testSuiteName, std::string_view testName,
                  std::function<void()> fun) {
        GlobalTestManager::getInstance().add(testSuiteName, std::string{testName}, std::move(fun));
    }

    // Leaves the registration logic to the caller
    TestRegistrar(auto&& registration) { registration(); }
};

// Test fixtures
class FixtureBase {
   public:
    virtual void SetUp() {}
    virtual void TearDown() {}
    virtual ~FixtureBase() = default;
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
#define TEST(TestSuite, Test)                                                               \
    static void TestSuite##_##Test##TestFun();                                              \
    const tst::TestRegistrar TestSuite##_##Test##_##Registrar(#TestSuite, #Test,            \
                                                              TestSuite##_##Test##TestFun); \
    static void TestSuite##_##Test##TestFun()

// Define a parametrized test function
#define TEST_P(TestSuite, Test, Parameter) static void TestSuite##_##Test##TestFun(Parameter)

// Instantiate a parametrized test function with a value for the parameter
// TODO: better interface and/or not a separate std::function object per parameter value
#define INSTANTIATE_TEST_SUITE_P(TestSuite, Test, ParameterValue)               \
    const tst::TestRegistrar TestSuite##_##Test##_##Registrar_##ParameterValue( \
        #TestSuite, #Test "/" #ParameterValue,                                  \
        []() { TestSuite##_##Test##TestFun(ParameterValue); })

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

namespace tst {
[[nodiscard]] inline bool cstrEqual(const char* s1, const char* s2) {
    if (s1 == nullptr) return s2 == nullptr;
    if (s2 == nullptr) return false;

    return std::strcmp(s1, s2) == 0;
}
// Case independent
[[nodiscard]] inline bool cstrIEqual(const char* s1, const char* s2) {
    if (s1 == nullptr) return s2 == nullptr;
    if (s2 == nullptr) return false;

    while (*s1 != '\0') {
        if (std::tolower(static_cast<unsigned char>(*s1)) !=
            std::tolower(static_cast<unsigned char>(*s2))) {
            return false;
        }
        ++s1;
        ++s2;
    }

    // Ensure both strings reached the end
    return *s1 == *s2;
}

// Implementation of floatAlmostEqual adapted from Bruce Dawson (Random ASCII blog)
template <typename Float>
[[nodiscard]] inline bool floatAlmostEqual(Float val1, Float val2) {
    static constexpr auto maxUlpsDiff = 4;
    static constexpr auto maxAbsDiff = std::numeric_limits<Float>::denorm_min() * maxUlpsDiff;

    // Comparing NaN always yields false
    if (std::isnan(val1) || std::isnan(val2)) return false;

    // Check if the numbers are really close - needed when comparing numbers near zero
    // Also takes care of comparing +0 to -0
    const auto absDiff = std::abs(val1 - val2);
    if (absDiff <= maxAbsDiff) return true;

    // Different signs means they do not match.
    // Also takes care of comparing +inf to -inf
    if (std::signbit(val1) != std::signbit(val2)) return false;

    // Find the difference in ULPs
    using Int = std::conditional_t<sizeof(Float) == 4, std::int32_t, std::int64_t>;
    static_assert(sizeof(Int) == sizeof(Float));

    const auto val1Int = std::bit_cast<Int>(val1);
    const auto val2Int = std::bit_cast<Int>(val2);
    // The ulpsDiff calculation cannot overflow since they have the same sign
    const auto ulpsDiff = std::abs(val1Int - val2Int);

    return ulpsDiff <= maxUlpsDiff;
}

// Implementation of testNear adapted from googletest's DoubleNearPredFormat
inline void testNear(const char* expr1, const char* expr2, const char* abs_error_expr, double val1,
                     double val2, double abs_error, FailureHandler f, const char* file, int line) {
    // We want to return success when the two values are infinity and at least one of the following
    // is true:
    //  - The values are the same-signed infinity.
    //  - The error limit itself is infinity.
    // This is done here so that we don't end up with a NaN when calculating the difference in
    // values.
    if (std::isinf(val1) && std::isinf(val2) &&
        (std::signbit(val1) == std::signbit(val2) || (abs_error > 0.0 && std::isinf(abs_error)))) {
        return;
    }

    const double diff = std::abs(val1 - val2);
    if (diff <= abs_error) return;

    // Find the value which is closest to zero.
    const double min_abs = std::min(std::abs(val1), std::abs(val2));
    // Find the distance to the next double from that value.
    const double epsilon =
        std::nextafter(min_abs, std::numeric_limits<double>::infinity()) - min_abs;
    // Detect the case where abs_error is so small that EXPECT_NEAR is effectively the same as
    // EXPECT_EQUAL, and give an informative error message so that the situation can be more easily
    // understood without requiring exotic floating-point knowledge.
    // Don't do an epsilon check if abs_error is zero because that implies that an equality check
    // was actually intended.
    if (!(std::isnan)(val1) && !(std::isnan)(val2) && abs_error > 0 && abs_error < epsilon) {
        std::cout
            << std::format(
                   "{8}({9}): error: The difference between {0} and {1} is {2}, where\n{0} "
                   "evaluates to {3},\n{1} evaluates to {4}.\nThe abs_error parameter {5} "
                   "evaluates to {6} which is smaller than "
                   "the minimum distance between doubles for numbers of this magnitude which is "
                   "{7}, thus "
                   "making this EXPECT_NEAR check equivalent to EXPECT_EQUAL. Consider using "
                   "EXPECT_DOUBLE_EQ instead.\n",
                   expr1, expr2, diff, val1, val2, abs_error_expr, abs_error, epsilon, file, line)
            << std::endl;
    } else {
        std::cout
            << std::format(
                   "{7}({8}): error: The difference between {0} and {1} is {2}, which exceeds "
                   "{5}, where\n{0} "
                   "evaluates to {3},\n{1} evaluates to {4}, and\n{5} evaluates to {6}.\n",
                   expr1, expr2, diff, val1, val2, abs_error_expr, abs_error, file, line)
            << std::endl;
    }

    // Call FailureHandler
    f();
}

}  // namespace tst

#define TEST_CSTRING(s1, s2, expected, FailureHandler)                                             \
    do {                                                                                           \
        TST_IF_UNLIKELY(tst::cstrEqual(s1, s2) != expected) {                                      \
            tst::predicateTestFailure(s1, s2, #s1, #s2, "==", FailureHandler, __FILE__, __LINE__); \
        }                                                                                          \
    } while (false)

#define TEST_CSTRING_ICASE(s1, s2, expected, FailureHandler)                                       \
    do {                                                                                           \
        TST_IF_UNLIKELY(tst::cstrIEqual(s1, s2) != expected) {                                     \
            tst::predicateTestFailure(s1, s2, #s1, #s2, "==", FailureHandler, __FILE__, __LINE__); \
        }                                                                                          \
    } while (false)

#define TEST_NEAR(s1, s2, expected, FailureHandler)                                                \
    do {                                                                                           \
        TST_IF_UNLIKELY(tst::cstrIEqual(s1, s2) != expected) {                                     \
            tst::predicateTestFailure(s1, s2, #s1, #s2, "==", FailureHandler, __FILE__, __LINE__); \
        }                                                                                          \
    } while (false)

#define TEST_FLOAT_EQ(f1, f2, fType, FailureHandler)                                               \
    do {                                                                                           \
        TST_IF_UNLIKELY(!tst::floatAlmostEqual<fType>(f1, f2)) {                                   \
            tst::predicateTestFailure(f1, f2, #f1, #f2, "==", FailureHandler, __FILE__, __LINE__); \
        }                                                                                          \
    } while (false)

#define TEST_FAIL(FailureHandler)                                                               \
    do {                                                                                        \
        std::cout << std::format("{}({}): error: Explicit test failure\n", __FILE__, __LINE__); \
        FailureHandler();                                                                       \
    } while (false)

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

// Exception assertions
#define EXPECT_THROW(statement, ExceptionType) \
    TEST_THROW(statement, const ExceptionType&, tst::nonFatalFailure)
#define EXPECT_ANY_THROW(statement) TEST_THROW(statement, ..., tst::nonFatalFailure)
#define EXPECT_NO_THROW(statement) TEST_NO_THROW(statement, tst::nonFatalFailure)
#define ASSERT_THROW(statement, ExceptionType) \
    TEST_THROW(statement, const ExceptionType&, tst::fatalFailure)
#define ASSERT_ANY_THROW(statement) TEST_THROW(statement, ..., tst::fatalFailure)
#define ASSERT_NO_THROW(statement) TEST_NO_THROW(statement, tst::fatalFailure)

// Useful together with SKIP_TEST
#define TST_SUPPRESS_UNREACHABLE_CODE_WARNING_BELOW(statement) \
    do {                                                       \
        if (tst::alwaysTrue()) {                               \
            statement;                                         \
        }                                                      \
    } while (false)

// Skip the current test
#define SKIP_TEST()                                                                     \
    do {                                                                                \
        std::cout << std::format("{}({}): Skipped\n", __FILE__, __LINE__) << std::endl; \
        TST_SUPPRESS_UNREACHABLE_CODE_WARNING_BELOW(throw tst::SkipTestException{});    \
    } while (false)

// C-string comparisons
#define EXPECT_STREQ(s1, s2) TEST_CSTRING(s1, s2, true, tst::nonFatalFailure)
#define EXPECT_STRNE(s1, s2) TEST_CSTRING(s1, s2, false, tst::nonFatalFailure)
#define EXPECT_STRCASEEQ(s1, s2) TEST_CSTRING_ICASE(s1, s2, true, tst::nonFatalFailure)
#define EXPECT_STRCASENE(s1, s2) TEST_CSTRING_ICASE(s1, s2, false, tst::nonFatalFailure)

#define ASSERT_STREQ(s1, s2) TEST_CSTRING(s1, s2, true, tst::fatalFailure)
#define ASSERT_STRNE(s1, s2) TEST_CSTRING(s1, s2, false, tst::fatalFailure)
#define ASSERT_STRCASEEQ(s1, s2) TEST_CSTRING_ICASE(s1, s2, true, tst::fatalFailure)
#define ASSERT_STRCASENE(s1, s2) TEST_CSTRING_ICASE(s1, s2, false, tst::fatalFailure)

// Floating-point comparisons: Test that two values are within 4 ULP, or both within 4 ULP of 0.
#define EXPECT_FLOAT_EQ(val1, val2) TEST_FLOAT_EQ(val1, val2, float, tst::nonFatalFailure)
#define EXPECT_DOUBLE_EQ(val1, val2) TEST_FLOAT_EQ(val1, val2, double, tst::nonFatalFailure)
#define ASSERT_FLOAT_EQ(val1, val2) TEST_FLOAT_EQ(val1, val2, float, tst::fatalFailure)
#define ASSERT_DOUBLE_EQ(val1, val2) TEST_FLOAT_EQ(val1, val2, double, tst::fatalFailure)

// Floating-point comparisons: Test that two values do not differ by more than abs_error
#define EXPECT_NEAR(val1, val2, abs_error)                                                         \
    tst::testNear(#val1, #val2, #abs_error, val1, val2, abs_error, tst::nonFatalFailure, __FILE__, \
                  __LINE__)

#define ASSERT_NEAR(val1, val2, abs_error) \
    tst::testNear(val1, val2, abs_error, tst::fatalFailure, __FILE__, __LINE__)

// The user-defined test function will be a member function of a type which inherits from the
// fixture type
#define TEST_F(TestFixture, Test)                                                           \
    class TestFixture##_##Test : TestFixture {                                              \
       public:                                                                              \
        TestFixture##_##Test() { SetUp(); }                                                 \
        ~TestFixture##_##Test() { TearDown(); }                                             \
        void tstFixtureFun();                                                               \
    };                                                                                      \
    const tst::TestRegistrar TestFixture##_##Test##_##Registrar(#TestFixture, #Test, []() { \
        TestFixture##_##Test t;                                                             \
        t.tstFixtureFun();                                                                  \
    });                                                                                     \
    void TestFixture##_##Test::tstFixtureFun()

// Helpers for typed tests
namespace tst {
// Store list of types
template <typename... Ts>
struct Types {};

// Count (comma-separated) types in a string of the form "int, long, short"
[[nodiscard]] consteval int countTypes(std::string_view str) {
    if (str.empty()) return 0;
    return static_cast<int>(std::count(str.begin(), str.end(), ',')) + 1;
}

// Return n-th type in a comma-separated type list
[[nodiscard]] consteval std::string_view findNthType(int n, std::string_view str) {
    auto start = std::size_t{0};
    auto end = static_cast<std::size_t>(-2);

    for (int i = 0; i <= n; ++i) {
        // Skip leading commas and spaces
        start = end + 2;
        if (start >= str.length()) return "";

        end = str.find(',', start);  // Find the next comma
        if (end == std::string_view::npos) end = str.size();
    }

    return str.substr(start, end - start);
}

// Registers the test function from the Fixture with each type in the type list Ts
template <template <typename> class Fixture, typename Array, typename... Ts>
void registerTyped(Types<Ts...>, std::string_view suiteName, std::string_view testName,
                   const Array& typeNameArray) {
    int typeNameIndex = 0;
    // Fold expression that creates lambdas for all test instances
    (
        [&] {
            auto fullTestName = std::string(testName) + "/";
            fullTestName += typeNameArray[typeNameIndex++];
            // Lambda that runs this particular test instance
            auto testBody = []() {
                Fixture<Ts> instance;
                instance.tstFixtureFun();
            };
            GlobalTestManager::getInstance().add(suiteName, std::move(fullTestName),
                                                 std::move(testBody));
        }(),
        ...);
}

// Create array of string_views for individual type names in the string str
template <size_t... Is>
consteval auto makeTypeNameArray(std::string_view str, std::index_sequence<Is...>) {
    return std::array<std::string_view, sizeof...(Is)>{findNthType(Is, str)...};
}
}  // namespace tst

// Declare all types for which the fixture test function will be run
#define TYPED_TEST_SUITE(FixtureName, ...)                                           \
    using FixtureName##_Types = tst::Types<__VA_ARGS__>;                             \
    static constexpr std::string_view FixtureName##_TypesString = #__VA_ARGS__;      \
    static constexpr auto FixtureName##_TypeNameArray = []() {                       \
        return tst::makeTypeNameArray(                                               \
            FixtureName##_TypesString,                                               \
            std::make_index_sequence<tst::countTypes(FixtureName##_TypesString)>{}); \
    }()

// Define a function of a typed test fixture
#define TYPED_TEST(FixtureName, TestName)                                                 \
    template <typename TypeParam>                                                         \
    class FixtureName##_##TestName##_Test : public FixtureName<TypeParam> {               \
       public:                                                                            \
        void tstFixtureFun();                                                             \
    };                                                                                    \
    const tst::TestRegistrar FixtureName##_##TestName##_Reg([]() {                        \
        tst::registerTyped<FixtureName##_##TestName##_Test>(                              \
            FixtureName##_Types{}, #FixtureName, #TestName, FixtureName##_TypeNameArray); \
    });                                                                                   \
    template <typename TypeParam>                                                         \
    void FixtureName##_##TestName##_Test<TypeParam>::tstFixtureFun()
