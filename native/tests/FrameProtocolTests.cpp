#include <doctest.h>

#include <cstring>

#include <mwb/FrameProtocol.h>

using namespace mwb::frame;

TEST_SUITE("FrameProtocol") {
    TEST_CASE("magic and fourcc spell MWBV and NV12 in memory order") {
        const FrameHeader header = MakeHeader(1280, 720, 0, 0, 0);
        unsigned char bytes[sizeof(FrameHeader)];
        std::memcpy(bytes, &header, sizeof(header));
        CHECK(std::memcmp(bytes, "MWBV", 4) == 0);
        CHECK(std::memcmp(bytes + 16, "NV12", 4) == 0);
    }

    TEST_CASE("MakeHeader produces a header that validates") {
        const FrameHeader header = MakeHeader(1920, 1080, kFlagPlaceholder, 42, 123456);
        CHECK(header.headerSize == kHeaderSize);
        CHECK(header.stride == 1920);
        CHECK(header.payloadSize == 1920u * 1080u * 3u / 2u);
        CHECK(header.seq == 42);
        CHECK(Validate(header, 1920, 1080) == ValidationError::None);
    }

    TEST_CASE("Validate rejects every malformed field") {
        const FrameHeader good = MakeHeader(1280, 720, 0, 1, 2);

        FrameHeader h = good;
        h.magic ^= 1;
        CHECK(Validate(h, 1280, 720) == ValidationError::BadMagic);

        h = good;
        h.version = 1;
        CHECK(Validate(h, 1280, 720) == ValidationError::BadVersion);

        h = good;
        h.headerSize = 64;
        CHECK(Validate(h, 1280, 720) == ValidationError::BadHeaderSize);

        h = good;
        h.fourcc = 0x32595559;  // YUY2
        CHECK(Validate(h, 1280, 720) == ValidationError::BadFourcc);

        CHECK(Validate(good, 1920, 1080) == ValidationError::ModeMismatch);

        h = good;
        h.stride = 1536;
        CHECK(Validate(h, 1280, 720) == ValidationError::BadStride);

        h = good;
        h.payloadSize -= 1;
        CHECK(Validate(h, 1280, 720) == ValidationError::BadPayloadSize);

        h = good;
        h.flags = 0x80;
        CHECK(Validate(h, 1280, 720) == ValidationError::UnknownFlags);
    }

    TEST_CASE("the catalog holds 20 modes and limits 4K to 30 fps") {
        CHECK(IsSupportedMode({1280, 720, 30, 1}));
        CHECK(IsSupportedMode({1920, 1080, 60, 1}));
        CHECK(IsSupportedMode({640, 480, 15, 1}));
        CHECK(IsSupportedMode({2560, 1440, 60, 1}));
        CHECK(IsSupportedMode({3840, 2160, 30, 1}));
        CHECK_FALSE(IsSupportedMode({3840, 2160, 60, 1}));
        CHECK_FALSE(IsSupportedMode({1280, 720, 30, 2}));
        CHECK_FALSE(IsSupportedMode({1280, 720, 29, 1}));
        CHECK_FALSE(IsSupportedMode({1280, 720, 24, 1}));
        CHECK_FALSE(IsSupportedMode({1024, 768, 30, 1}));
        CHECK_FALSE(IsSupportedMode({0, 0, 30, 1}));

        std::size_t count = 0;
        for (const FrameSize& size : kCatalogSizes) {
            for (const std::uint32_t fps : kCatalogFps) count += IsSupportedMode({size.width, size.height, fps, 1}) ? 1 : 0;
        }
        CHECK(count == 20);
        CHECK(AdvertisedModes(kFullCatalogCap, {1920, 1080, 30, 1}).size() == 20);
    }

    TEST_CASE("advertised modes start with the default mode and respect the cap") {
        const ModeList all = AdvertisedModes(kFullCatalogCap, {1920, 1080, 30, 1});
        CHECK(all[0] == VideoMode{1920, 1080, 30, 1});
        CHECK(all[1] == VideoMode{3840, 2160, 30, 1});
        CHECK(all[2] == VideoMode{3840, 2160, 15, 1});
        CHECK(all[3] == VideoMode{2560, 1440, 60, 1});
        CHECK(all[all.size() - 1] == VideoMode{640, 360, 15, 1});
        CHECK(all.Contains({1920, 1080, 30, 1}));

        const ModeCap cap{1920, 1080, 30};
        const ModeList capped = AdvertisedModes(cap, {1280, 720, 30, 1});
        CHECK(capped.size() == 10);  // 5 sizes x {30, 15}
        CHECK(capped[0] == VideoMode{1280, 720, 30, 1});
        CHECK(capped[1] == VideoMode{1920, 1080, 30, 1});
        for (const VideoMode& mode : capped) {
            CHECK(mode.width <= 1920);
            CHECK(mode.fpsNum <= 30);
        }

        // A default mode outside the cap is not advertised.
        const ModeList withoutDefault = AdvertisedModes(cap, {3840, 2160, 30, 1});
        CHECK_FALSE(withoutDefault.Contains({3840, 2160, 30, 1}));
        CHECK(withoutDefault[0] == VideoMode{1920, 1080, 30, 1});
    }

    TEST_CASE("a selected format maps back to its advertised mode") {
        const ModeList modes = AdvertisedModes(kFullCatalogCap, {1920, 1080, 30, 1});
        CHECK(FindMode(modes, 1280, 720, 60, 1) == VideoMode{1280, 720, 60, 1});
        CHECK(FindMode(modes, 1280, 720, 120, 2) == VideoMode{1280, 720, 60, 1});
        CHECK(FindMode(modes, 1920, 1080, 0, 0) == VideoMode{1920, 1080, 30, 1});  // unspecified: first listed
        CHECK_FALSE(FindMode(modes, 3840, 2160, 60, 1).has_value());
        CHECK_FALSE(FindMode(modes, 1024, 768, 30, 1).has_value());

        CHECK(FindModeByDuration(modes, 1280, 720, 333'333) == VideoMode{1280, 720, 30, 1});
        CHECK(FindModeByDuration(modes, 1280, 720, 333'334) == VideoMode{1280, 720, 30, 1});
        CHECK(FindModeByDuration(modes, 1280, 720, 166'667) == VideoMode{1280, 720, 60, 1});
        CHECK(FindModeByDuration(modes, 640, 480, 666'667) == VideoMode{640, 480, 15, 1});
        CHECK(FindModeByDuration(modes, 1920, 1080, 0) == VideoMode{1920, 1080, 30, 1});
        CHECK_FALSE(FindModeByDuration(modes, 1280, 720, 400'000).has_value());  // 25 fps
    }

    TEST_CASE("caps must use a catalog frame rate and admit the smallest size") {
        CHECK(IsValidCap(kFullCatalogCap));
        CHECK(IsValidCap({640, 360, 15}));
        CHECK_FALSE(IsValidCap({640, 360, 25}));
        CHECK_FALSE(IsValidCap({320, 240, 30}));
        CHECK_FALSE(IsValidCap({7680, 4320, 30}));
    }

    TEST_CASE("subscription requests spell MWBS and validate against the cap") {
        const SubscribeRequest request = MakeSubscribeRequest({1280, 720, 30, 1});
        unsigned char bytes[sizeof(SubscribeRequest)];
        std::memcpy(bytes, &request, sizeof(request));
        CHECK(std::memcmp(bytes, "MWBS", 4) == 0);
        CHECK(request.version == kVersion);
        CHECK(request.size == kSubscribeSize);
        CHECK(ModeOf(request) == VideoMode{1280, 720, 30, 1});
        CHECK(ValidateSubscribe(request, kFullCatalogCap) == SubscribeError::None);

        SubscribeRequest r = request;
        r.magic ^= 1;
        CHECK(ValidateSubscribe(r, kFullCatalogCap) == SubscribeError::BadMagic);

        r = request;
        r.version = 1;
        CHECK(ValidateSubscribe(r, kFullCatalogCap) == SubscribeError::BadVersion);

        r = request;
        r.size = 48;
        CHECK(ValidateSubscribe(r, kFullCatalogCap) == SubscribeError::BadSize);

        r = MakeSubscribeRequest({1024, 768, 30, 1});
        CHECK(ValidateSubscribe(r, kFullCatalogCap) == SubscribeError::NotAdvertised);

        r = MakeSubscribeRequest({1920, 1080, 60, 1});
        CHECK(ValidateSubscribe(r, {1920, 1080, 30}) == SubscribeError::NotAdvertised);
        CHECK(ValidateSubscribe(r, kFullCatalogCap) == SubscribeError::None);
    }
}
