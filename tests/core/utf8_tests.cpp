#include "kessoku/core/utf8.h"

#include <cstdio>
#include <string>

namespace {

int gFailures = 0;

void Check(bool condition, const char* testName, const char* desc,
           int line) {
    if (!condition) {
        ++gFailures;
        fprintf(stderr, "FAIL: %s (%s) at line %d\n", testName, desc, line);
    } else {
        printf("PASS: %s (%s)\n", testName, desc);
    }
}

} // namespace

#define CHECK(cond, desc) Check((cond), __func__, (desc), __LINE__)

int main() {
    using kessoku::core::Utf8ToWide;
    using kessoku::core::WideToUtf8;

    // Empty input is empty output in both directions.
    CHECK(WideToUtf8(L"").empty(), "empty wide -> empty utf8");
    CHECK(Utf8ToWide("").empty(), "empty utf8 -> empty wide");

    // ASCII is unchanged.
    CHECK(WideToUtf8(L"Kessoku") == "Kessoku", "ascii round-trips to utf8");
    CHECK(Utf8ToWide("Kessoku") == L"Kessoku", "ascii round-trips to wide");

    // BMP non-ASCII: U+00E9 (e-acute) encodes as C3 A9 in UTF-8.
    const std::wstring bmp = L"caf\u00E9";
    const std::string bmpUtf8 = "caf\xC3\xA9";
    CHECK(WideToUtf8(bmp) == bmpUtf8, "BMP character encodes as UTF-8");
    CHECK(Utf8ToWide(bmpUtf8) == bmp, "BMP character decodes from UTF-8");

    // Non-BMP: U+1D11E (musical G clef) is a surrogate pair in UTF-16 and
    // F0 9D 84 9E in UTF-8.
    const std::wstring astral = L"\U0001D11E";
    const std::string astralUtf8 = "\xF0\x9D\x84\x9E";
    CHECK(WideToUtf8(astral) == astralUtf8,
          "non-BMP character encodes as UTF-8");
    CHECK(Utf8ToWide(astralUtf8) == astral,
          "non-BMP character decodes from UTF-8");

    // Long input exercises the two-call size negotiation.
    const std::wstring longText(4096, L'\u00E9');
    CHECK(Utf8ToWide(WideToUtf8(longText)) == longText,
          "long non-ASCII text round-trips");

    if (gFailures == 0) {
        printf("\n=== Results: 0 failures ===\n");
        return 0;
    }
    printf("\n=== Results: %d failures ===\n", gFailures);
    return 1;
}
