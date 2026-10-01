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
        h.version = 2;
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

    TEST_CASE("only whitelisted camera modes are supported") {
        CHECK(IsSupportedMode({1280, 720, 30, 1}));
        CHECK(IsSupportedMode({1920, 1080, 60, 1}));
        CHECK(IsSupportedMode({640, 360, 15, 1}));
        CHECK_FALSE(IsSupportedMode({1280, 720, 30, 2}));
        CHECK_FALSE(IsSupportedMode({1280, 720, 29, 1}));
        CHECK_FALSE(IsSupportedMode({1024, 768, 30, 1}));
        CHECK_FALSE(IsSupportedMode({0, 0, 30, 1}));
    }
}
