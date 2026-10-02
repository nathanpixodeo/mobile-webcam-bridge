// Unit tests for the hub's NV12 scaler (fit + letterbox, box pre-reduction, bilinear).
#if __has_include(<doctest/doctest.h>)
#include <doctest/doctest.h>
#else
#include <doctest.h>
#endif

#include <mwb/FrameProtocol.h>
#include <mwb/um/Nv12Scaler.h>

#include <cmath>
#include <cstdint>
#include <tuple>
#include <vector>

namespace color = mwb::um::color;

namespace {

struct Frame {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint8_t> bytes;

    Frame(std::uint32_t w, std::uint32_t h) : width(w), height(h), bytes(mwb::frame::Nv12FrameBytes(w, h)) {}

    std::uint8_t& Y(std::uint32_t x, std::uint32_t y) { return bytes[std::size_t{y} * width + x]; }
    std::uint8_t& U(std::uint32_t x, std::uint32_t y) { return bytes[Chroma(x, y)]; }
    std::uint8_t& V(std::uint32_t x, std::uint32_t y) { return bytes[Chroma(x, y) + 1]; }
    [[nodiscard]] color::Nv12Image View() const { return color::PackedNv12(bytes.data(), width, height); }

private:
    [[nodiscard]] std::size_t Chroma(std::uint32_t x, std::uint32_t y) const {
        return std::size_t{width} * height + std::size_t{y} * width + 2 * std::size_t{x};
    }
};

Frame Solid(std::uint32_t w, std::uint32_t h, std::uint8_t y, std::uint8_t u, std::uint8_t v) {
    Frame frame(w, h);
    for (std::uint32_t row = 0; row < h; ++row) {
        for (std::uint32_t x = 0; x < w; ++x) frame.Y(x, row) = y;
    }
    for (std::uint32_t row = 0; row < h / 2; ++row) {
        for (std::uint32_t x = 0; x < w / 2; ++x) {
            frame.U(x, row) = u;
            frame.V(x, row) = v;
        }
    }
    return frame;
}

Frame Scaled(const Frame& src, std::uint32_t w, std::uint32_t h) {
    Frame out(w, h);
    color::Nv12Scaler scaler(src.width, src.height, w, h);
    scaler.Scale(src.View(), out.bytes.data());
    return out;
}

}  // namespace

TEST_CASE("Nv12Scaler copies a frame of the same size unchanged") {
    Frame src(8, 4);
    for (std::size_t i = 0; i < src.bytes.size(); ++i) src.bytes[i] = static_cast<std::uint8_t>(i * 7);
    const Frame out = Scaled(src, 8, 4);
    CHECK(out.bytes == src.bytes);
}

TEST_CASE("Nv12Scaler halves exactly with a 2x2 box filter") {
    Frame src(8, 4);
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t x = 0; x < 8; ++x) src.Y(x, row) = static_cast<std::uint8_t>(16 + 10 * x + 40 * row);
    }
    for (std::uint32_t row = 0; row < 2; ++row) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            src.U(x, row) = static_cast<std::uint8_t>(100 + 4 * x + 20 * row);
            src.V(x, row) = static_cast<std::uint8_t>(150 - 4 * x);
        }
    }

    Frame out = Scaled(src, 4, 2);
    // Y(0,0) = average of 16, 26, 56, 66 = 41.
    CHECK(out.Y(0, 0) == 41);
    CHECK(out.Y(3, 1) == (src.Y(6, 2) + src.Y(7, 2) + src.Y(6, 3) + src.Y(7, 3) + 2) / 4);
    // U(0,0) = average of 100, 104, 120, 124 = 112; V(1,0) = average of 142, 138, 142, 138 = 140.
    CHECK(out.U(0, 0) == 112);
    CHECK(out.V(1, 0) == 140);
}

TEST_CASE("Nv12Scaler keeps flat colours flat when upscaling and downscaling") {
    const Frame src = Solid(64, 36, 180, 90, 200);
    for (const auto& [w, h] : {std::pair{128u, 72u}, std::pair{32u, 18u}, std::pair{96u, 54u}, std::pair{160u, 90u}}) {
        Frame out = Scaled(src, w, h);
        CHECK(out.Y(0, 0) == 180);
        CHECK(out.Y(w - 1, h - 1) == 180);
        CHECK(out.Y(w / 2, h / 2) == 180);
        CHECK(out.U(w / 4, h / 4) == 90);
        CHECK(out.V(w / 2 - 1, h / 2 - 1) == 200);
    }
}

TEST_CASE("Nv12Scaler interpolates a horizontal gradient monotonically") {
    Frame src(16, 2);
    for (std::uint32_t x = 0; x < 16; ++x) {
        src.Y(x, 0) = static_cast<std::uint8_t>(16 + 12 * x);
        src.Y(x, 1) = static_cast<std::uint8_t>(16 + 12 * x);
    }
    Frame out = Scaled(src, 48, 6);
    for (std::uint32_t x = 1; x < 48; ++x) CHECK(out.Y(x, 3) >= out.Y(x - 1, 3));
    CHECK(out.Y(0, 3) == 16);
    CHECK(out.Y(47, 3) == 16 + 12 * 15);
}

TEST_CASE("Nv12Scaler letterboxes 16:9 into 4:3 with black bars at the top and bottom") {
    const Frame src = Solid(1920, 1080, 200, 60, 190);
    color::Nv12Scaler scaler(1920, 1080, 640, 480);
    CHECK(scaler.Content().x == 0);
    CHECK(scaler.Content().y == 60);
    CHECK(scaler.Content().width == 640);
    CHECK(scaler.Content().height == 360);

    Frame out(640, 480);
    scaler.Scale(src.View(), out.bytes.data());
    CHECK(out.Y(320, 0) == 16);
    CHECK(out.Y(320, 59) == 16);
    CHECK(out.Y(320, 60) == 200);
    CHECK(out.Y(320, 419) == 200);
    CHECK(out.Y(320, 420) == 16);
    CHECK(out.U(160, 0) == 128);
    CHECK(out.V(160, 239) == 128);
    CHECK(out.U(160, 30) == 60);
    CHECK(out.V(160, 209) == 190);
}

TEST_CASE("Nv12Scaler pillarboxes 4:3 into 16:9 with bars on the sides") {
    const Frame src = Solid(640, 480, 120, 70, 170);
    color::Nv12Scaler scaler(640, 480, 1920, 1080);
    CHECK(scaler.Content().x == 240);
    CHECK(scaler.Content().width == 1440);
    CHECK(scaler.Content().y == 0);

    Frame out(1920, 1080);
    scaler.Scale(src.View(), out.bytes.data());
    CHECK(out.Y(239, 500) == 16);
    CHECK(out.Y(240, 500) == 120);
    CHECK(out.Y(1679, 500) == 120);
    CHECK(out.Y(1680, 500) == 16);
    CHECK(out.U(119, 250) == 128);
    CHECK(out.U(120, 250) == 70);
}

TEST_CASE("Nv12Scaler reduces 4K to 1080p with one box pass and no bilinear blur") {
    Frame src(3840, 2160);
    for (std::uint32_t row = 0; row < 2160; ++row) {
        for (std::uint32_t x = 0; x < 3840; ++x) src.Y(x, row) = static_cast<std::uint8_t>(16 + (x / 2) % 200);
    }
    for (std::size_t i = std::size_t{3840} * 2160; i < src.bytes.size(); ++i) src.bytes[i] = 128;

    Frame out = Scaled(src, 1920, 1080);
    for (std::uint32_t x = 0; x < 1920; x += 97) CHECK(out.Y(x, 540) == 16 + x % 200);
}

TEST_CASE("Nv12Scaler bilinear stays within two code values of an exact reference") {
    // Exact bilinear with the same pixel-centre alignment, in floating point.
    const auto reference = [](const std::vector<std::uint8_t>& plane, std::uint32_t srcW, std::uint32_t srcH,
                              std::uint32_t dstW, std::uint32_t dstH, std::uint32_t x, std::uint32_t y) {
        const auto axis = [](std::uint32_t i, std::uint32_t src, std::uint32_t dst) {
            double position = (i + 0.5) * src / dst - 0.5;
            if (position < 0) position = 0;
            const auto first = static_cast<std::uint32_t>(position);
            const std::uint32_t second = first + 1 < src ? first + 1 : first;
            return std::tuple{first, second, position - first};
        };
        const auto [x0, x1, fx] = axis(x, srcW, dstW);
        const auto [y0, y1, fy] = axis(y, srcH, dstH);
        const auto at = [&](std::uint32_t px, std::uint32_t py) { return static_cast<double>(plane[py * srcW + px]); };
        const double top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
        const double bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
        return top * (1 - fy) + bottom * fy;
    };

    Frame src(64, 36);
    for (std::uint32_t row = 0; row < 36; ++row) {
        for (std::uint32_t x = 0; x < 64; ++x) src.Y(x, row) = static_cast<std::uint8_t>((x * 37 + row * 91 + x * row) % 220 + 16);
    }
    for (std::size_t i = std::size_t{64} * 36; i < src.bytes.size(); ++i) src.bytes[i] = 128;
    const std::vector<std::uint8_t> luma(src.bytes.begin(), src.bytes.begin() + 64 * 36);

    Frame out = Scaled(src, 160, 90);
    int worst = 0;
    for (std::uint32_t y = 0; y < 90; ++y) {
        for (std::uint32_t x = 0; x < 160; ++x) {
            const double expected = reference(luma, 64, 36, 160, 90, x, y);
            const int difference = static_cast<int>(std::lround(std::abs(out.Y(x, y) - expected)));
            worst = difference > worst ? difference : worst;
        }
    }
    CHECK(worst <= 2);
}
