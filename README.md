# tst
Single-header C++20 testing framework with syntax and output similar to googletest.

Less feature-rich, but also only ~1% the size. And it doesn't trip up the
LLVM TypeSanitizer (which is the reason I implemented this).

## Usage

```cpp
#include <cmath>
#include <stdexcept>
#include <vector>
#include "tst/tst.hpp"

// Simple unit test
// Automatically registers itself
TEST(SuiteName, TestName) {
    int i = 1;
    EXPECT_EQ(i, 1);
    EXPECT_TRUE(true);

    // Floating point comparisons based on ULP difference
    EXPECT_FLOAT_EQ(0.1f, std::nextafter(0.1f, 1.f));

    // Floating point comparisons based on absolute error
    EXPECT_NEAR(0.1, 0.1f, 0.000001);

    // Safe C-String comparisons
    EXPECT_STRCASEEQ("abc", "ABC");

    // Test if exceptions are thrown
    std::vector<int> v;
    EXPECT_THROW(i = v.at(0), std::out_of_range);
    ASSERT_NO_THROW(v.push_back(i));

    // Explicit failure
    ADD_FAILURE();
}

// Parametrized unit test
TEST_P(SuiteName, ParamTest, int i) {
    EXPECT_GE(i, 0);

    // Skipping tests is possible
    SKIP_TEST();
}
INSTANTIATE_TEST_SUITE_P(SuiteName, ParamTest, 0);
INSTANTIATE_TEST_SUITE_P(SuiteName, ParamTest, 1);

int main() {
    // Call this once from anywhere
    GlobalTestManager::getInstance().runAllTests();
}
```

## Roadmap (Maybe)
- More expressive parametrized tests
- Fixtures and typed tests

## Intentional Omissions
- Death tests
- Mocking and matchers
