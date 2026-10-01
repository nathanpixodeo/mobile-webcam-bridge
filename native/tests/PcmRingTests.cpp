#include <doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include <mwb/PcmRing.h>

using mwb::PcmRing;

namespace {

std::vector<unsigned char> Sequence(unsigned first, unsigned count) {
    std::vector<unsigned char> bytes(count);
    for (unsigned i = 0; i < count; ++i) bytes[i] = static_cast<unsigned char>(first + i);
    return bytes;
}

}  // namespace

TEST_SUITE("PcmRing") {
    TEST_CASE("attach validates its arguments") {
        std::array<unsigned char, 16> storage{};
        PcmRing ring;
        CHECK_FALSE(ring.Attach(nullptr, 16, 2));
        CHECK_FALSE(ring.Attach(storage.data(), 15, 2));  // not a multiple of the block
        CHECK_FALSE(ring.Attach(storage.data(), 16, 0));
        CHECK(ring.Attach(storage.data(), 16, 2));
        CHECK(ring.Capacity() == 16);
        CHECK(ring.BufferedBytes() == 0);
    }

    TEST_CASE("bytes come out in order across the wrap point") {
        std::array<unsigned char, 8> storage{};
        PcmRing ring;
        REQUIRE(ring.Attach(storage.data(), 8, 2));

        const auto first = Sequence(1, 6);
        CHECK(ring.WriteDropOldest(first.data(), 6) == 0);
        std::array<unsigned char, 4> out{};
        CHECK(ring.ReadZeroFill(out.data(), 4) == 4);
        CHECK(out == std::array<unsigned char, 4>{1, 2, 3, 4});

        const auto second = Sequence(7, 6);  // wraps around the end of storage
        CHECK(ring.WriteDropOldest(second.data(), 6) == 0);
        CHECK(ring.BufferedBytes() == 8);
        std::array<unsigned char, 8> all{};
        CHECK(ring.ReadZeroFill(all.data(), 8) == 8);
        CHECK(all == std::array<unsigned char, 8>{5, 6, 7, 8, 9, 10, 11, 12});
    }

    TEST_CASE("overrun drops the oldest audio and reports it") {
        std::array<unsigned char, 8> storage{};
        PcmRing ring;
        REQUIRE(ring.Attach(storage.data(), 8, 2));

        const auto first = Sequence(1, 6);
        REQUIRE(ring.WriteDropOldest(first.data(), 6) == 0);
        const auto second = Sequence(7, 4);
        CHECK(ring.WriteDropOldest(second.data(), 4) == 2);  // bytes 1 and 2 dropped
        std::array<unsigned char, 8> out{};
        CHECK(ring.ReadZeroFill(out.data(), 8) == 8);
        CHECK(out == std::array<unsigned char, 8>{3, 4, 5, 6, 7, 8, 9, 10});
    }

    TEST_CASE("a write larger than the ring keeps only its newest bytes") {
        std::array<unsigned char, 4> storage{};
        PcmRing ring;
        REQUIRE(ring.Attach(storage.data(), 4, 2));
        const auto data = Sequence(1, 10);
        CHECK(ring.WriteDropOldest(data.data(), 10) == 6);
        std::array<unsigned char, 4> out{};
        CHECK(ring.ReadZeroFill(out.data(), 4) == 4);
        CHECK(out == std::array<unsigned char, 4>{7, 8, 9, 10});
    }

    TEST_CASE("underrun is zero-filled") {
        std::array<unsigned char, 8> storage{};
        PcmRing ring;
        REQUIRE(ring.Attach(storage.data(), 8, 2));
        const auto data = Sequence(1, 2);
        REQUIRE(ring.WriteDropOldest(data.data(), 2) == 0);
        std::array<unsigned char, 6> out{9, 9, 9, 9, 9, 9};
        CHECK(ring.ReadZeroFill(out.data(), 6) == 2);
        CHECK(out == std::array<unsigned char, 6>{1, 2, 0, 0, 0, 0});
        CHECK(ring.BufferedBytes() == 0);
    }

    TEST_CASE("partial audio frames are never stored") {
        std::array<unsigned char, 8> storage{};
        PcmRing ring;
        REQUIRE(ring.Attach(storage.data(), 8, 2));
        const auto data = Sequence(1, 5);
        CHECK(ring.WriteDropOldest(data.data(), 5) == 0);
        CHECK(ring.BufferedBytes() == 4);  // the odd trailing byte is ignored
    }

    TEST_CASE("clear and detach empty the ring") {
        std::array<unsigned char, 8> storage{};
        PcmRing ring;
        REQUIRE(ring.Attach(storage.data(), 8, 2));
        const auto data = Sequence(1, 4);
        REQUIRE(ring.WriteDropOldest(data.data(), 4) == 0);
        ring.Clear();
        CHECK(ring.BufferedBytes() == 0);
        ring.Detach();
        CHECK_FALSE(ring.IsAttached());
        std::array<unsigned char, 2> out{5, 5};
        CHECK(ring.ReadZeroFill(out.data(), 2) == 0);
        CHECK(out == std::array<unsigned char, 2>{0, 0});
    }
}
