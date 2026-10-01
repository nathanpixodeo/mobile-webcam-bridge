// Pure logic of the video hub: frame slicing, latest-frame-wins slots, output policy, commands.
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "video/ConsumerSlot.h"
#include "video/FrameAssembler.h"
#include "video/HubCommand.h"
#include "video/OutputPolicy.h"

using namespace mwb::native;

TEST_SUITE("FrameAssembler") {
    TEST_CASE("slices a stream into whole frames regardless of chunking") {
        constexpr std::size_t kFrame = 6;
        std::vector<std::uint8_t> stream(kFrame * 3 + 2);  // three frames and a partial one
        std::iota(stream.begin(), stream.end(), static_cast<std::uint8_t>(0));

        for (std::size_t chunk = 1; chunk <= stream.size(); ++chunk) {
            FrameAssembler assembler(kFrame, nullptr);
            std::vector<std::vector<std::uint8_t>> frames;
            for (std::size_t offset = 0; offset < stream.size(); offset += chunk) {
                const std::size_t count = std::min(chunk, stream.size() - offset);
                assembler.Feed(std::span<const std::uint8_t>(stream.data() + offset, count),
                               [&](std::vector<std::uint8_t>&& frame) { frames.push_back(std::move(frame)); });
            }
            REQUIRE(frames.size() == 3);
            for (std::size_t i = 0; i < frames.size(); ++i) {
                CHECK(frames[i].size() == kFrame);
                CHECK(frames[i].front() == static_cast<std::uint8_t>(i * kFrame));
            }
            CHECK(assembler.PendingBytes() == 2);
        }
    }

    TEST_CASE("reset drops the partial frame") {
        FrameAssembler assembler(4, nullptr);
        std::vector<std::vector<std::uint8_t>> frames;
        const auto collect = [&](std::vector<std::uint8_t>&& frame) { frames.push_back(std::move(frame)); };
        const std::uint8_t first[] = {1, 2, 3};
        assembler.Feed(first, collect);
        assembler.Reset();
        const std::uint8_t second[] = {5, 6, 7, 8};
        assembler.Feed(second, collect);
        REQUIRE(frames.size() == 1);
        CHECK(frames[0] == std::vector<std::uint8_t>{5, 6, 7, 8});
    }

    TEST_CASE("buffers come from the supplied source") {
        int requests = 0;
        FrameAssembler assembler(2, [&] {
            ++requests;
            return FrameAssembler::Buffer();
        });
        const std::uint8_t data[] = {1, 2, 3, 4};
        assembler.Feed(data, [](std::vector<std::uint8_t>&&) {});
        CHECK(requests == 2);
    }

    TEST_CASE("committing past the frame is an error") {
        FrameAssembler assembler(4, nullptr);
        (void)assembler.WritableRegion();
        CHECK_THROWS_AS((void)assembler.Commit(5), std::out_of_range);
    }
}

TEST_SUITE("ConsumerSlot") {
    TEST_CASE("keeps only the newest item and reports replacements") {
        ConsumerSlot<int> slot;
        CHECK_FALSE(slot.HasPending());
        CHECK_FALSE(slot.Offer(1));
        CHECK(slot.Offer(2));  // 1 was never sent: replaced
        CHECK(slot.Offer(3));
        CHECK(slot.Take() == 3);
        CHECK_FALSE(slot.Take().has_value());
        CHECK_FALSE(slot.Offer(4));
        CHECK(slot.HasPending());
    }
}

TEST_SUITE("OutputPolicy") {
    TEST_CASE("live only while ingest frames keep arriving") {
        OutputPolicy policy(300);
        CHECK(policy.Decide(0) == OutputDecision{OutputMode::Placeholder, PlaceholderKind::Stopped});
        policy.OnIngestFrame(1000);
        CHECK(policy.Decide(1000).mode == OutputMode::Live);
        CHECK(policy.Decide(1300).mode == OutputMode::Live);
        CHECK(policy.Decide(1301) == OutputDecision{OutputMode::Placeholder, PlaceholderKind::Stopped});
    }

    TEST_CASE("a commanded placeholder wins until cleared") {
        OutputPolicy policy(300);
        policy.OnIngestFrame(1000);
        policy.Command(PlaceholderKind::Background);
        CHECK(policy.Decide(1001) == OutputDecision{OutputMode::Placeholder, PlaceholderKind::Background});
        policy.Command(std::nullopt);
        CHECK(policy.Decide(1002).mode == OutputMode::Live);
    }

    TEST_CASE("kinds round-trip through their names") {
        for (const PlaceholderKind kind : {PlaceholderKind::NoDevice, PlaceholderKind::AppClosed, PlaceholderKind::Paused,
                                           PlaceholderKind::Background, PlaceholderKind::Stopped}) {
            CHECK(ParsePlaceholderKind(ToString(kind)) == kind);
        }
        CHECK_FALSE(ParsePlaceholderKind("idle").has_value());
    }
}

TEST_SUITE("HubCommand") {
    TEST_CASE("placeholder with a kind or null") {
        const HubCommand show = ParseHubCommand(R"({"cmd":"placeholder","kind":"no-device"})");
        REQUIRE(std::holds_alternative<ShowPlaceholderCommand>(show));
        CHECK(std::get<ShowPlaceholderCommand>(show).kind == PlaceholderKind::NoDevice);

        const HubCommand live = ParseHubCommand(R"({"cmd":"placeholder","kind":null,"future":1})");
        REQUIRE(std::holds_alternative<ShowPlaceholderCommand>(live));
        CHECK_FALSE(std::get<ShowPlaceholderCommand>(live).kind.has_value());
    }

    TEST_CASE("loadPlaceholder carries a UTF-8 path") {
        const HubCommand load =
            ParseHubCommand(R"({"cmd":"loadPlaceholder","kind":"paused","path":"C:\\cache\\caf\u00e9.nv12"})");
        REQUIRE(std::holds_alternative<LoadPlaceholderCommand>(load));
        const auto& command = std::get<LoadPlaceholderCommand>(load);
        CHECK(command.kind == PlaceholderKind::Paused);
        CHECK(command.path == std::wstring(L"C:\\cache\\caf\u00e9.nv12"));
    }

    TEST_CASE("malformed commands are rejected") {
        CHECK_THROWS_AS((void)ParseHubCommand("not json"), HubCommandError);
        CHECK_THROWS_AS((void)ParseHubCommand(R"(["placeholder"])"), HubCommandError);
        CHECK_THROWS_AS((void)ParseHubCommand(R"({"kind":"paused"})"), HubCommandError);
        CHECK_THROWS_AS((void)ParseHubCommand(R"({"cmd":"placeholder"})"), HubCommandError);
        CHECK_THROWS_AS((void)ParseHubCommand(R"({"cmd":"placeholder","kind":"idle"})"), HubCommandError);
        CHECK_THROWS_AS((void)ParseHubCommand(R"({"cmd":"placeholder","kind":3})"), HubCommandError);
        CHECK_THROWS_AS((void)ParseHubCommand(R"({"cmd":"loadPlaceholder","kind":"paused"})"), HubCommandError);
        CHECK_THROWS_AS((void)ParseHubCommand(R"({"cmd":"loadPlaceholder","kind":"paused","path":""})"), HubCommandError);
        CHECK_THROWS_AS((void)ParseHubCommand(R"({"cmd":"explode"})"), HubCommandError);
    }
}
