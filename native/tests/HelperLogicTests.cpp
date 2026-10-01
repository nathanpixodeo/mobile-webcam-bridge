// Pure logic of the mic feeder, the stdin line splitter and the install options.
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/ArgParser.h"
#include "core/Errors.h"
#include "install/InstallOptions.h"
#include "mic/PcmChunker.h"
#include "platform/OsVersion.h"
#include "platform/Stdin.h"

using namespace mwb::native;

TEST_SUITE("PcmChunker") {
    TEST_CASE("emits fixed chunks across arbitrary reads") {
        PcmChunker chunker(4, 2);
        std::vector<std::vector<std::uint8_t>> chunks;
        const auto collect = [&](std::span<const std::uint8_t> chunk) { chunks.emplace_back(chunk.begin(), chunk.end()); };

        std::vector<std::uint8_t> data(11);
        std::iota(data.begin(), data.end(), static_cast<std::uint8_t>(1));
        chunker.Push(std::span<const std::uint8_t>(data.data(), 3), collect);
        chunker.Push(std::span<const std::uint8_t>(data.data() + 3, 8), collect);
        REQUIRE(chunks.size() == 2);
        CHECK(chunks[0] == std::vector<std::uint8_t>{1, 2, 3, 4});
        CHECK(chunks[1] == std::vector<std::uint8_t>{5, 6, 7, 8});
        CHECK(chunker.PendingBytes() == 3);

        chunker.Flush(collect);  // 9, 10 are a whole sample; 11 is half of one
        REQUIRE(chunks.size() == 3);
        CHECK(chunks[2] == std::vector<std::uint8_t>{9, 10});
        CHECK(chunker.PendingBytes() == 0);
    }

    TEST_CASE("chunk size must be a multiple of the block") {
        CHECK_THROWS_AS(PcmChunker(5, 2), std::invalid_argument);
    }
}

TEST_SUITE("LineSplitter") {
    TEST_CASE("splits lines, strips CR and reports overlong lines") {
        LineSplitter splitter(8);
        std::vector<std::pair<std::string, bool>> lines;
        const auto collect = [&](std::string_view line, bool overflow) { lines.emplace_back(std::string(line), overflow); };
        splitter.Push("ab\r\ncd", collect);
        splitter.Push("\n0123456789\nok\n", collect);
        REQUIRE(lines.size() == 4);
        CHECK(lines[0] == std::pair<std::string, bool>{"ab", false});
        CHECK(lines[1] == std::pair<std::string, bool>{"cd", false});
        CHECK(lines[2].second);
        CHECK(lines[3] == std::pair<std::string, bool>{"ok", false});
        CHECK_FALSE(splitter.HasPartialLine());
    }
}

TEST_SUITE("InstallOptions") {
    TEST_CASE("defaults: auto camera, mic, 1280x720@30, name Mobile Webcam") {
        const InstallOptions options = ToInstallOptions(InstallArgParser().Parse({}));
        CHECK(options.camera == CameraChoice::Auto);
        CHECK(options.mic);
        CHECK(options.mode == mwb::frame::VideoMode{1280, 720, 30, 1});
        CHECK(options.friendlyName == L"Mobile Webcam");
    }

    TEST_CASE("explicit options are validated") {
        const std::vector<std::wstring> args{L"--camera", L"dshow", L"--no-mic", L"--width", L"1920",
                                             L"--height", L"1080",  L"--fps",    L"60",      L"--name",
                                             L"Desk Cam"};
        const InstallOptions options = ToInstallOptions(InstallArgParser().Parse(args));
        CHECK(options.camera == CameraChoice::DirectShow);
        CHECK_FALSE(options.mic);
        CHECK(options.mode == mwb::frame::VideoMode{1920, 1080, 60, 1});
        CHECK(options.friendlyName == L"Desk Cam");

        const auto rejects = [](std::vector<std::wstring> bad) {
            CHECK_THROWS_AS((void)ToInstallOptions(InstallArgParser().Parse(bad)), CommandError);
        };
        rejects({L"--camera", L"usb"});
        rejects({L"--mic", L"--no-mic"});
        rejects({L"--width", L"1280"});
        rejects({L"--width", L"1024", L"--height", L"768"});
        rejects({L"--fps", L"29"});
        rejects({L"--name", L"bad\"name"});
    }

    TEST_CASE("backend resolution follows the OS build") {
        const OsVersion windows10{10, 0, 19045};
        const OsVersion windows11{10, 0, 26200};
        CHECK(ResolveCameraBackend(CameraChoice::Auto, windows11) == CameraBackend::MediaFoundation);
        CHECK(ResolveCameraBackend(CameraChoice::Auto, windows10) == CameraBackend::DirectShow);
        CHECK(ResolveCameraBackend(CameraChoice::DirectShow, windows11) == CameraBackend::DirectShow);
        CHECK(ResolveCameraBackend(CameraChoice::None, windows11) == CameraBackend::None);
        try {
            (void)ResolveCameraBackend(CameraChoice::MediaFoundation, windows10);
            FAIL("expected NOT_SUPPORTED_OS");
        } catch (const CommandError& error) {
            CHECK(error.Code() == ErrorCode::NotSupportedOs);
            CHECK(error.Exit() == ExitCode::NotSupportedOs);
        }
    }

    TEST_CASE("uninstall without flags removes everything") {
        CHECK(ToUninstallOptions(UninstallArgParser().Parse({})).Everything());
        const UninstallOptions micOnly = ToUninstallOptions(UninstallArgParser().Parse(std::vector<std::wstring>{L"--mic"}));
        CHECK(micOnly.mic);
        CHECK_FALSE(micOnly.camera);
    }
}
