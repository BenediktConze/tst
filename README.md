Small testing framework with syntax and output similar to googletest.

Less feature-rich, but also only ~0.5% the size. And it doesn't trip up the
TypeSanitizer (which is the reason I implemented this).

Limitations:
- No fixtures or typed tests
- No death tests
- No string or floating point assertions
- No mocking or matchers
