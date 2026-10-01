// Unit tests for mwb::TripleBuffer and the small pure helpers of common-um.
#if __has_include(<doctest/doctest.h>)
#include <doctest/doctest.h>
#else
#include <doctest.h>
#endif

#include <mwb/Identifiers.h>
#include <mwb/TripleBuffer.h>
#include <mwb/um/CameraConfig.h>
#include <mwb/um/Guid.h>

#include <atomic>
#include <cstdint>
#include <thread>

TEST_CASE("TripleBuffer starts empty and delivers the newest published value") {
    mwb::TripleBuffer<int> buffer;
    CHECK_FALSE(buffer.HasFresh());
    CHECK_FALSE(buffer.Acquire());

    buffer.WriteSlot() = 1;
    buffer.Publish();
    buffer.WriteSlot() = 2;
    buffer.Publish();  // 1 is superseded before the consumer looks

    CHECK(buffer.HasFresh());
    CHECK(buffer.Acquire());
    CHECK(buffer.ReadSlot() == 2);
    CHECK_FALSE(buffer.HasFresh());
    CHECK_FALSE(buffer.Acquire());
    CHECK(buffer.ReadSlot() == 2);  // read slot stays stable until the next Acquire
}

TEST_CASE("TripleBuffer never hands the producer the slot the consumer is reading") {
    mwb::TripleBuffer<int> buffer;
    buffer.WriteSlot() = 10;
    buffer.Publish();
    REQUIRE(buffer.Acquire());
    const int* reading = &buffer.ReadSlot();

    for (int i = 0; i < 10; ++i) {
        CHECK(&buffer.WriteSlot() != reading);
        buffer.WriteSlot() = i;
        buffer.Publish();
    }
    CHECK(*reading == 10);
}

TEST_CASE("TripleBuffer delivers monotonically increasing values across threads") {
    struct Sample {
        std::uint64_t value = 0;
        std::uint64_t check = 0;  // must always equal ~value: detects torn slots
    };
    mwb::TripleBuffer<Sample> buffer;
    constexpr std::uint64_t kCount = 200'000;
    std::atomic<bool> done{false};

    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= kCount; ++i) {
            Sample& slot = buffer.WriteSlot();
            slot.value = i;
            slot.check = ~i;
            buffer.Publish();
        }
        done = true;
    });

    std::uint64_t last = 0;
    bool consistent = true;
    while (!done || buffer.HasFresh()) {
        if (buffer.Acquire()) {
            const Sample& sample = buffer.ReadSlot();
            consistent = consistent && sample.check == ~sample.value && sample.value > last;
            last = sample.value;
        }
    }
    producer.join();

    CHECK(consistent);
    CHECK(last == kCount);
}

TEST_CASE("GuidFromString parses the product CLSIDs") {
    constexpr GUID source = mwb::um::GuidFromString(mwb::ids::kMfSourceClsid);
    static_assert(source.Data1 == 0x39F0E089u);
    static_assert(source.Data2 == 0xE94D);
    static_assert(source.Data3 == 0x41FA);
    static_assert(source.Data4[0] == 0xBD && source.Data4[1] == 0x58);
    static_assert(source.Data4[7] == 0x52);

    constexpr GUID filter = mwb::um::GuidFromString(mwb::ids::kDShowFilterClsid);
    CHECK(filter.Data1 == 0x4FE59245u);
    CHECK(filter.Data4[7] == 0x7D);
}

TEST_CASE("Pipe names from the registry are whitelisted") {
    CHECK(mwb::um::IsValidPipeName(L"mobile-webcam-bridge-video"));
    CHECK(mwb::um::IsValidPipeName(L"a.b_c-1"));
    CHECK_FALSE(mwb::um::IsValidPipeName(L""));
    CHECK_FALSE(mwb::um::IsValidPipeName(L"..\\evil"));
    CHECK_FALSE(mwb::um::IsValidPipeName(L"with space"));
    CHECK_FALSE(mwb::um::IsValidPipeName(std::wstring(129, L'a')));
    CHECK(mwb::um::PipePath(L"x") == L"\\\\.\\pipe\\x");
}
