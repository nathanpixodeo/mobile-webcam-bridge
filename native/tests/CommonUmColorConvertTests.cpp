// Unit tests for common-um colour conversion and the placeholder generator.
#if __has_include(<doctest/doctest.h>)
#include <doctest/doctest.h>
#else
#include <doctest.h>
#endif

#include <mwb/um/ColorConvert.h>
#include <mwb/um/Placeholder.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace color = mwb::um::color;

namespace {

// 4×2 NV12 image: Y row 0 = 16 32 48 64, Y row 1 = 80 96 112 128, UV = (100,150) (200,50).
std::vector<std::uint8_t> MakeTinyNv12() {
    return {16, 32, 48, 64, 80, 96, 112, 128, 100, 150, 200, 50};
}

// Reference BT.709 limited-range conversion in floating point.
color::Rgb Reference(std::uint8_t y, std::uint8_t u, std::uint8_t v) {
    const double c = 1.164383 * (y - 16);
    const double d = u - 128.0;
    const double e = v - 128.0;
    auto clamp = [](double value) {
        return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : std::lround(value)));
    };
    return {clamp(c + 1.792741 * e), clamp(c - 0.213249 * d - 0.532909 * e), clamp(c + 2.112402 * d)};
}

bool Near(std::uint8_t a, std::uint8_t b) {
    return (a > b ? a - b : b - a) <= 1;
}

}  // namespace

TEST_CASE("YuvToRgb maps limited-range black and white to full-range black and white") {
    const color::Rgb black = color::YuvToRgb(16, 128, 128);
    CHECK(black.r == 0);
    CHECK(black.g == 0);
    CHECK(black.b == 0);

    const color::Rgb white = color::YuvToRgb(235, 128, 128);
    CHECK(white.r == 255);
    CHECK(white.g == 255);
    CHECK(white.b == 255);
}

TEST_CASE("YuvToRgb stays within one code value of the BT.709 reference") {
    for (int y = 16; y <= 235; y += 17) {
        for (int u = 16; u <= 240; u += 28) {
            for (int v = 16; v <= 240; v += 28) {
                const auto y8 = static_cast<std::uint8_t>(y);
                const auto u8 = static_cast<std::uint8_t>(u);
                const auto v8 = static_cast<std::uint8_t>(v);
                const color::Rgb actual = color::YuvToRgb(y8, u8, v8);
                const color::Rgb expected = Reference(y8, u8, v8);
                CHECK(Near(actual.r, expected.r));
                CHECK(Near(actual.g, expected.g));
                CHECK(Near(actual.b, expected.b));
            }
        }
    }
}

TEST_CASE("CopyNv12 honours a padded destination pitch") {
    const std::vector<std::uint8_t> source = MakeTinyNv12();
    const auto image = color::PackedNv12(source.data(), 4, 2);
    constexpr std::size_t pitch = 8;
    std::vector<std::uint8_t> destination(pitch * 3, 0xEE);

    color::CopyNv12(image, destination.data(), pitch);

    CHECK(destination[0] == 16);
    CHECK(destination[3] == 64);
    CHECK(destination[4] == 0xEE);  // padding untouched
    CHECK(destination[pitch + 0] == 80);
    CHECK(destination[2 * pitch + 0] == 100);  // UV plane starts at pitch × height
    CHECK(destination[2 * pitch + 3] == 50);
}

TEST_CASE("Nv12ToYuy2 interleaves luma with the shared chroma pair") {
    const std::vector<std::uint8_t> source = MakeTinyNv12();
    const auto image = color::PackedNv12(source.data(), 4, 2);
    std::vector<std::uint8_t> destination(4 * 2 * 2);

    color::Nv12ToYuy2(image, destination.data(), 8);

    const std::vector<std::uint8_t> expected = {
        16, 100, 32, 150, 48, 200, 64, 50,     // row 0
        80, 100, 96, 150, 112, 200, 128, 50,   // row 1 reuses the chroma row
    };
    CHECK(destination == expected);
}

TEST_CASE("Nv12ToI420 de-interleaves chroma into U and V planes") {
    const std::vector<std::uint8_t> source = MakeTinyNv12();
    const auto image = color::PackedNv12(source.data(), 4, 2);
    std::vector<std::uint8_t> destination(12);

    color::Nv12ToI420(image, destination.data(), 4);

    const std::vector<std::uint8_t> expected = {16, 32, 48, 64, 80, 96, 112, 128, 100, 200, 150, 50};
    CHECK(destination == expected);
}

TEST_CASE("Nv12ToRgb24BottomUp writes BGR rows bottom-up with DWORD-aligned pitch") {
    const std::vector<std::uint8_t> source = MakeTinyNv12();
    const auto image = color::PackedNv12(source.data(), 4, 2);
    const std::size_t pitch = color::Rgb24Pitch(4);
    REQUIRE(pitch == 12);
    std::vector<std::uint8_t> destination(pitch * 2);

    color::Nv12ToRgb24BottomUp(image, destination.data(), pitch);

    // Top-left source pixel (Y=16, U=100, V=150) lands in the last row of the DIB.
    const color::Rgb topLeft = color::YuvToRgb(16, 100, 150);
    CHECK(destination[pitch + 0] == topLeft.b);
    CHECK(destination[pitch + 1] == topLeft.g);
    CHECK(destination[pitch + 2] == topLeft.r);

    // Bottom-right source pixel (Y=128, U=200, V=50) lands at the end of the first DIB row.
    const color::Rgb bottomRight = color::YuvToRgb(128, 200, 50);
    CHECK(destination[9] == bottomRight.b);
    CHECK(destination[10] == bottomRight.g);
    CHECK(destination[11] == bottomRight.r);
}

TEST_CASE("Rgb24Pitch rounds rows up to whole DWORDs") {
    CHECK(color::Rgb24Pitch(1) == 4);
    CHECK(color::Rgb24Pitch(4) == 12);
    CHECK(color::Rgb24Pitch(5) == 16);
    CHECK(color::Rgb24Pitch(1280) == 3840);
}

TEST_CASE("Placeholder is dark, neutral, and its bar sweeps across the frame") {
    const mwb::um::PlaceholderGenerator generator(1280, 720, 30);
    std::vector<std::uint8_t> frame(generator.FrameBytes());
    REQUIRE(frame.size() == 1280u * 720u * 3u / 2u);

    generator.Render(0, frame.data());
    CHECK(frame[generator.BarWidth() + 1] == mwb::um::PlaceholderGenerator::kBackgroundLuma);
    CHECK(frame[0] == mwb::um::PlaceholderGenerator::kBarLuma);  // bar starts at column 0
    CHECK(frame[1280u * 720u] == mwb::um::PlaceholderGenerator::kNeutralChroma);
    CHECK(frame.back() == mwb::um::PlaceholderGenerator::kNeutralChroma);

    const std::uint32_t halfway = generator.BarOffset(60);  // 2 s into a 4 s sweep
    CHECK(halfway > 500);
    CHECK(halfway < 700);
    CHECK(halfway % 2 == 0);  // aligned to NV12 chroma pairs
    CHECK(generator.BarOffset(120) == 0);  // wraps after one sweep
}
