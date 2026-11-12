#include "kessoku/core/utf8.h"

#include <cstdint>
#include <cstdio>
#include <random>
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

    // Embedded NUL is data, not a terminator: both conversions are
    // length-driven, so it must survive unchanged in both directions.
    const std::wstring embeddedWide(L"a\0b", 3);
    const std::string embeddedUtf8("a\0b", 3);
    CHECK(WideToUtf8(embeddedWide) == embeddedUtf8,
          "embedded NUL survives wide -> utf8");
    CHECK(Utf8ToWide(embeddedUtf8) == embeddedWide,
          "embedded NUL survives utf8 -> wide");

    // Every valid BMP scalar value must round-trip. Surrogates are excluded
    // because lone surrogates are not scalar values in UTF-8.
    {
        std::wstring allBmp;
        allBmp.reserve(0xD7FF + 0x10000 - 0xE000);
        for (uint32_t cp = 0x20; cp <= 0xD7FF; ++cp) {
            allBmp.push_back(static_cast<wchar_t>(cp));
        }
        for (uint32_t cp = 0xE000; cp <= 0xFFFD; ++cp) {
            allBmp.push_back(static_cast<wchar_t>(cp));
        }
        CHECK(Utf8ToWide(WideToUtf8(allBmp)) == allBmp,
              "all valid BMP scalar values round-trip");
    }

    // Deterministic non-BMP coverage: surrogate pairs and four-byte UTF-8.
    {
        std::mt19937 rng(0x5EED1234u);
        std::wstring randomAstral;
        for (int i = 0; i < 4096; ++i) {
            const uint32_t cp =
                0x10000u + static_cast<uint32_t>(rng() % 0x100000u);
            const uint32_t value = cp - 0x10000u;
            randomAstral.push_back(static_cast<wchar_t>(
                0xD800u + (value >> 10)));
            randomAstral.push_back(static_cast<wchar_t>(
                0xDC00u + (value & 0x3FFu)));
        }
        CHECK(Utf8ToWide(WideToUtf8(randomAstral)) == randomAstral,
              "random non-BMP scalar values round-trip");
    }

    // Exact scalar boundaries: the last BMP value before the surrogate gap,
    // the first value after it, the replacement character, and the first and
    // last non-BMP scalar values.
    {
        std::wstring boundaries;
        boundaries.push_back(static_cast<wchar_t>(0xD7FF));
        boundaries.push_back(static_cast<wchar_t>(0xE000));
        boundaries.push_back(static_cast<wchar_t>(0xFFFD));

        const uint32_t astralValues[] = {0x10000u, 0x10FFFFu};
        for (const uint32_t cp : astralValues) {
            const uint32_t value = cp - 0x10000u;
            boundaries.push_back(
                static_cast<wchar_t>(0xD800u + (value >> 10)));
            boundaries.push_back(
                static_cast<wchar_t>(0xDC00u + (value & 0x3FFu)));
        }

        CHECK(Utf8ToWide(WideToUtf8(boundaries)) == boundaries,
              "UTF-16 and UTF-8 scalar boundaries round-trip");
    }

    // Malformed UTF-8 must never crash or read past the supplied length.
    // Windows replaces invalid sequences with U+FFFD because the conversion
    // deliberately does not request MB_ERR_INVALID_CHARS.
    const std::string invalidSequences[] = {
        "\xFF",
        "\xC3",
        "\xC0\xAF",
        "\xC0\x80",
        "\xE0\x80\x80",
        "\xF0\x9D\x84",
        "\x80\x80\x80",
        "\xED\xA0\x80",
        "\xF4\x90\x80\x80",
        "\xFB\xBF\xBF\xBF\xBF",
        "\xFE\xFE",
    };
    for (const std::string& invalid : invalidSequences) {
        const std::wstring converted = Utf8ToWide(invalid);
        CHECK(!converted.empty(), "malformed UTF-8 converts to replacement text");
        CHECK(converted.find(L'\uFFFD') != std::wstring::npos,
              "malformed UTF-8 sequence contains U+FFFD");
    }
    CHECK(Utf8ToWide(std::string("\xFF", 1)).find(L'\uFFFD') !=
              std::wstring::npos,
          "invalid UTF-8 byte becomes U+FFFD");

    // Invalid bytes surrounded by valid text must not discard the valid text.
    const std::wstring mixed = Utf8ToWide(std::string("ok\xFFok", 5));
    CHECK(mixed.find(L"ok") != std::wstring::npos,
          "valid text around an invalid byte survives conversion");
    CHECK(mixed.find(L'\uFFFD') != std::wstring::npos,
          "invalid byte is replaced inside valid text");

    // A lone surrogate is not encodable UTF-8; the converter must replace it
    // rather than emit an invalid byte sequence or crash.
    const std::wstring loneSurrogate(1, static_cast<wchar_t>(0xD800));
    const std::string surrogateUtf8 = WideToUtf8(loneSurrogate);
    CHECK(!surrogateUtf8.empty(), "lone surrogate encodes to replacement text");
    CHECK(surrogateUtf8 == "\xEF\xBF\xBD",
          "lone surrogate becomes U+FFFD in UTF-8");

    const std::wstring loneLowSurrogate(1, static_cast<wchar_t>(0xDC00));
    CHECK(WideToUtf8(loneLowSurrogate) == "\xEF\xBF\xBD",
          "lone low surrogate becomes U+FFFD in UTF-8");

    // A high surrogate followed by a non-low-surrogate is not a valid pair.
    const std::wstring brokenPair = {
        static_cast<wchar_t>(0xD800), L'A'};
    const std::wstring decodedPair = Utf8ToWide(WideToUtf8(brokenPair));
    CHECK(decodedPair.find(L'\uFFFD') != std::wstring::npos,
          "unpaired high surrogate becomes U+FFFD");
    CHECK(decodedPair.find(L'A') != std::wstring::npos,
          "text after an unpaired surrogate survives");

    if (gFailures == 0) {
        printf("\n=== Results: 0 failures ===\n");
        return 0;
    }
    printf("\n=== Results: %d failures ===\n", gFailures);
    return 1;
}
