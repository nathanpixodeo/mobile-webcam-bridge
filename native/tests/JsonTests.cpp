#include <doctest.h>

#include <string>

#include "core/Json.h"

using namespace mwb::native;

TEST_SUITE("JsonWriter") {
    TEST_CASE("writes compact nested documents") {
        JsonWriter writer;
        writer.BeginObject()
            .Field("ok", true)
            .Field("count", static_cast<std::uint32_t>(3))
            .Field("delta", static_cast<std::int64_t>(-7))
            .Key("items")
            .BeginArray()
            .String(std::string_view("a"))
            .Null()
            .EndArray()
            .Key("nested")
            .BeginObject()
            .Field("x", false)
            .EndObject()
            .EndObject();
        CHECK(writer.Take() == R"({"ok":true,"count":3,"delta":-7,"items":["a",null],"nested":{"x":false}})");
    }

    TEST_CASE("escapes quotes, backslashes and control characters, not slashes") {
        JsonWriter writer;
        writer.String(std::string_view("a\"b\\c/d\n\t\x01"));
        CHECK(writer.Take() == R"("a\"b\\c/d\n\t\u0001")");
    }

    TEST_CASE("passes valid UTF-8 through and replaces invalid bytes") {
        JsonWriter valid;
        valid.String(std::string_view("caf\xC3\xA9"));
        CHECK(valid.Take() == "\"caf\xC3\xA9\"");

        JsonWriter invalid;
        invalid.String(std::string_view("a\xFF" "b"));
        CHECK(invalid.Take() == R"("a\ufffdb")");
    }

    TEST_CASE("converts UTF-16 including surrogate pairs") {
        JsonWriter writer;
        writer.String(std::wstring_view(L"C:\\Users\\\u00e9\U0001F600"));
        CHECK(writer.Take() == "\"C:\\\\Users\\\\\xC3\xA9\xF0\x9F\x98\x80\"");
    }

    TEST_CASE("misuse is a logic error") {
        JsonWriter writer;
        writer.BeginObject();
        CHECK_THROWS_AS(writer.String(std::string_view("no key")), std::logic_error);
        JsonWriter incomplete;
        incomplete.BeginArray();
        CHECK_THROWS_AS((void)incomplete.Take(), std::logic_error);
    }
}

TEST_SUITE("JsonParser") {
    TEST_CASE("parses objects, arrays and scalars") {
        const JsonValue value = ParseJson(R"( {"cmd":"placeholder","kind":null,"n":-1.5e2,"list":[true,false,"x"]} )");
        REQUIRE(value.AsObject() != nullptr);
        CHECK(*value.Find("cmd")->AsString() == "placeholder");
        CHECK(value.Find("kind")->IsNull());
        CHECK(*value.Find("n")->AsNumber() == doctest::Approx(-150.0));
        const JsonArray& list = *value.Find("list")->AsArray();
        REQUIRE(list.size() == 3);
        CHECK(*list[0].AsBool());
        CHECK_FALSE(*list[1].AsBool());
        CHECK(*list[2].AsString() == "x");
        CHECK(value.Find("missing") == nullptr);
    }

    TEST_CASE("decodes escapes and surrogate pairs") {
        const JsonValue value = ParseJson(R"("a\"\\\/\n\u00e9\ud83d\ude00")");
        CHECK(*value.AsString() == "a\"\\/\n\xC3\xA9\xF0\x9F\x98\x80");
    }

    TEST_CASE("rejects malformed input") {
        CHECK_THROWS_AS(ParseJson("{\"a\":1,}"), JsonParseError);         // trailing comma
        CHECK_THROWS_AS(ParseJson("{\"a\":1,\"a\":2}"), JsonParseError);  // duplicate key
        CHECK_THROWS_AS(ParseJson("[1] x"), JsonParseError);              // trailing data
        CHECK_THROWS_AS(ParseJson("\"\\ud83d\""), JsonParseError);        // lone surrogate
        CHECK_THROWS_AS(ParseJson("\"tab\there\""), JsonParseError);      // raw control character
        CHECK_THROWS_AS(ParseJson("01"), JsonParseError);                 // leading zero
        CHECK_THROWS_AS(ParseJson("\"\xFF\""), JsonParseError);           // invalid UTF-8
        CHECK_THROWS_AS(ParseJson("// comment\n1"), JsonParseError);
    }

    TEST_CASE("enforces depth and size limits") {
        CHECK_THROWS_AS(ParseJson("[[[[1]]]]", JsonParseLimits{3, 1024}), JsonParseError);
        CHECK_NOTHROW((void)ParseJson("[[[1]]]", JsonParseLimits{3, 1024}));
        CHECK_THROWS_AS(ParseJson("\"0123456789\"", JsonParseLimits{16, 8}), JsonParseError);
    }
}
