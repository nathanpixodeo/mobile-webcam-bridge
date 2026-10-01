#include <doctest.h>

#include <string>
#include <vector>

#include "core/ArgParser.h"
#include "core/Errors.h"

using namespace mwb::native;

namespace {

ArgParser Parser() {
    return ArgParser{{L"width", true}, {L"mic", false}, {L"name", true}, {L"elevated", false, true}};
}

ParsedOptions Parse(std::vector<std::wstring> args) { return Parser().Parse(args); }

ErrorCode CodeOf(std::vector<std::wstring> args) {
    try {
        (void)Parse(std::move(args));
    } catch (const CommandError& error) {
        return error.Code();
    }
    return ErrorCode::Internal;
}

}  // namespace

TEST_SUITE("ArgParser") {
    TEST_CASE("accepts separate and inline values and flags") {
        const ParsedOptions options = Parse({L"--width", L"1280", L"--name=My Cam", L"--mic"});
        CHECK(options.Value(L"width") == std::wstring(L"1280"));
        CHECK(options.Value(L"name") == std::wstring(L"My Cam"));
        CHECK(options.Has(L"mic"));
        CHECK_FALSE(options.Has(L"elevated"));
        CHECK(options.UInt32(L"width", 1, 4000) == 1280u);
    }

    TEST_CASE("rejects unknown, repeated and malformed options") {
        CHECK(CodeOf({L"--nope"}) == ErrorCode::Usage);
        CHECK(CodeOf({L"--mic", L"--mic"}) == ErrorCode::Usage);
        CHECK(CodeOf({L"--mic=1"}) == ErrorCode::Usage);
        CHECK(CodeOf({L"--width"}) == ErrorCode::Usage);
        CHECK(CodeOf({L"--width", L"--mic"}) == ErrorCode::Usage);
        CHECK(CodeOf({L"stray"}) == ErrorCode::Usage);
        CHECK(CodeOf({L"--"}) == ErrorCode::Usage);
    }

    TEST_CASE("typed accessors validate ranges") {
        const ParsedOptions options = Parse({L"--width", L"99999"});
        CHECK_THROWS_AS((void)options.UInt32(L"width", 1, 4000), CommandError);
        const ParsedOptions missing = Parse({});
        CHECK_FALSE(missing.UInt32(L"width", 1, 4000).has_value());
        CHECK_THROWS_AS((void)missing.Required(L"name"), CommandError);
    }

    TEST_CASE("ParseDecimalUInt32 is strict") {
        CHECK(ParseDecimalUInt32(L"0") == 0u);
        CHECK(ParseDecimalUInt32(L"4294967295") == 4294967295u);
        CHECK_FALSE(ParseDecimalUInt32(L"4294967296").has_value());
        CHECK_FALSE(ParseDecimalUInt32(L"-1").has_value());
        CHECK_FALSE(ParseDecimalUInt32(L"+1").has_value());
        CHECK_FALSE(ParseDecimalUInt32(L"1e3").has_value());
        CHECK_FALSE(ParseDecimalUInt32(L"").has_value());
    }
}
