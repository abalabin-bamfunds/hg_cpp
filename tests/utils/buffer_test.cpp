#include <bg/util/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include "counting_memory_resource.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

TEST_CASE("a contiguous buffer exposes committed bytes", "[coverage-exempt]") {
    auto data = bg::util::buffer::contiguous(8);
    bg::util::buffer_writer writer{data};
    auto reservation = writer.reserve(3);
    std::memcpy(reservation.bytes().data(), "abc", 3);

    CHECK(data.size() == 0);
    CHECK(data.bytes().empty());

    reservation.commit();
    CHECK(data.type() == bg::util::buffer_type::contiguous);
    CHECK(data.size() == 3);
    CHECK(std::memcmp(data.bytes().data(), "abc", 3) == 0);
}

TEST_CASE("an unsized write commits a contiguous buffer", "[coverage-exempt]") {
    auto data = bg::util::buffer::contiguous(4);
    bg::util::buffer_writer writer{data};
    constexpr auto value = std::string_view{"abc"};
    writer.write_bytes(std::as_bytes(std::span{value.data(), value.size()}));

    CHECK(data.bytes().empty());
    writer.commit();
    CHECK(std::memcmp(data.bytes().data(), "abc", 3) == 0);

    constexpr auto too_large = std::string_view{"de"};
    CHECK_THROWS_AS(writer.write_bytes(std::as_bytes(std::span{too_large.data(), too_large.size()})),
                    std::length_error);
    CHECK(data.size() == 3);
}

TEST_CASE("an unsized write spanning blocks becomes readable on commit", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(3);
    bg::util::buffer_writer writer{data};
    bg::util::buffer_reader reader{data};
    constexpr auto value = std::string_view{"abcdefgh"};
    writer.write_bytes(std::as_bytes(std::span{value.data(), value.size()}));

    CHECK(reader.readable().empty());
    writer.commit();

    for (const auto part : {std::string_view{"abc"}, std::string_view{"def"}, std::string_view{"gh"}}) {
        const auto available = reader.readable();
        REQUIRE(available.size() == part.size());
        CHECK(std::memcmp(available.data(), part.data(), part.size()) == 0);
        reader.consume(part.size());
    }
    CHECK(reader.readable().empty());
}

TEST_CASE("a fixed-size write copies within the current block", "[coverage-exempt]") {
    auto data = bg::util::buffer::contiguous(8);
    bg::util::buffer_writer writer{data};
    const auto expected = std::array{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                     std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};

    writer.write_bytes(std::span{std::as_const(expected)});
    writer.commit();

    CHECK(data.size() == expected.size());
    CHECK(std::memcmp(data.bytes().data(), expected.data(), expected.size()) == 0);
}

TEST_CASE("a fixed-size write falls back to split a chain write", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(4);
    bg::util::buffer_writer writer{data};
    bg::util::buffer_reader reader{data};
    const auto expected = std::array{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5},
                                     std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}};

    writer.write_bytes(std::span{std::as_const(expected)});
    CHECK(reader.readable().empty());
    writer.commit();

    auto actual = std::array<std::byte, expected.size()>{};
    reader.read_bytes(actual);
    CHECK(actual == expected);
}

TEST_CASE("a fixed-size write rejects insufficient contiguous capacity", "[coverage-exempt]") {
    auto data = bg::util::buffer::contiguous(8);
    bg::util::buffer_writer writer{data};
    const auto bytes = std::array<std::byte, 9>{};

    CHECK_THROWS_AS(writer.write_bytes(std::span{std::as_const(bytes)}), std::length_error);
    writer.commit();
    CHECK(data.size() == 0);
}

TEST_CASE("a buffer writer rolls back an uncommitted chain write", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(16);
    bg::util::buffer_writer writer{data};
    writer.write(std::uint64_t{1});
    writer.commit();

    writer.write_bytes(std::as_bytes(std::span{"abandoned write spanning blocks"}));
    writer.rollback();
    writer.write(std::uint64_t{2});
    writer.commit();

    bg::util::buffer_reader reader{data};
    CHECK(reader.read<std::uint64_t>() == 1);
    CHECK(reader.read<std::uint64_t>() == 2);
    CHECK(reader.readable().empty());
}

TEST_CASE("rollback cleanup reads an uncommitted chain write across blocks", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(8);
    bg::util::buffer_writer writer{data};
    const auto committed = std::string_view{"keep"};
    writer.write_bytes(std::as_bytes(std::span{committed.data(), committed.size()}));
    writer.commit();

    const auto pending = std::string_view{"this pending payload spans several chain blocks"};
    writer.write_bytes(std::as_bytes(std::span{pending.data(), pending.size()}));

    auto cleaned_size = std::size_t{};
    writer.rollback([&](bg::util::buffer_reader &reader) {
        CHECK_THROWS_AS(reader.fork(), std::logic_error);
        while (true) {
            const auto bytes = reader.readable();
            if (bytes.empty()) {
                break;
            }

            cleaned_size += bytes.size();
            reader.consume(bytes.size());
        }
    });

    CHECK(cleaned_size == pending.size());

    const auto next = std::string_view{"next"};
    writer.write_bytes(std::as_bytes(std::span{next.data(), next.size()}));
    writer.commit();

    auto reader = bg::util::buffer_reader{data};
    std::array<char, 8> result{};
    reader.read_bytes(std::as_writable_bytes(std::span{result}));
    CHECK(std::string_view{result.data(), result.size()} == "keepnext");
    CHECK(reader.readable().empty());
}

TEST_CASE("a buffer writer rolls back an uncommitted contiguous write", "[coverage-exempt]") {
    auto data = bg::util::buffer::contiguous(128);
    bg::util::buffer_writer writer{data};
    writer.write(std::uint64_t{1});
    writer.commit();

    writer.write(std::uint64_t{2});
    writer.rollback();
    writer.write(std::uint64_t{3});
    writer.commit();

    bg::util::buffer_reader reader{data};
    CHECK(reader.read<std::uint64_t>() == 1);
    CHECK(reader.read<std::uint64_t>() == 3);
    CHECK(reader.readable().empty());
}

TEST_CASE("an abandoned unsized write releases private blocks", "[coverage-exempt]") {
    auto memory = counting_memory_resource{};
    {
        auto data = bg::util::buffer::chain(2, bg::allocator{memory});
        {
            bg::util::buffer_writer writer{data};
            constexpr auto value = std::string_view{"abcde"};
            writer.write_bytes(std::as_bytes(std::span{value.data(), value.size()}));
            CHECK(memory.allocations() == 3);
        }

        CHECK(memory.allocations() == 1);
        bg::util::buffer_reader reader{data};
        CHECK(reader.readable().empty());
    }
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an unsized rollover waits for its starting block to commit", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(3);
    bg::util::buffer_writer writer{data};
    bg::util::buffer_reader reader{data};
    constexpr auto first = std::string_view{"x"};
    writer.write_bytes(std::as_bytes(std::span{first.data(), first.size()}));
    writer.commit();
    reader.consume(1);

    constexpr auto second = std::string_view{"abcd"};
    writer.write_bytes(std::as_bytes(std::span{second.data(), second.size()}));
    CHECK(reader.readable().empty());
    writer.commit();

    const auto current = reader.readable();
    REQUIRE(current.size() == 2);
    CHECK(std::memcmp(current.data(), "ab", 2) == 0);
    reader.consume(2);
    const auto next = reader.readable();
    REQUIRE(next.size() == 2);
    CHECK(std::memcmp(next.data(), "cd", 2) == 0);
}

TEST_CASE("an unsized rollover from a full block publishes its link on commit", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(3);
    bg::util::buffer_writer writer{data};
    bg::util::buffer_reader reader{data};
    constexpr auto first = std::string_view{"abc"};
    writer.write_bytes(std::as_bytes(std::span{first.data(), first.size()}));
    writer.commit();
    reader.consume(3);

    constexpr auto second = std::string_view{"de"};
    writer.write_bytes(std::as_bytes(std::span{second.data(), second.size()}));
    CHECK(reader.readable().empty());
    writer.commit();
    const auto available = reader.readable();
    REQUIRE(available.size() == 2);
    CHECK(std::memcmp(available.data(), "de", 2) == 0);
}

TEST_CASE("a concurrent reader sees no unsized write before commit", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(3);
    bg::util::buffer_reader reader{data};
    auto sync = std::barrier{2};
    constexpr auto write_count = 100;
    constexpr auto write_size = 7;

    auto producer = std::thread{[&] {
        bg::util::buffer_writer writer{data};
        for (auto index = 0; index < write_count; ++index) {
            auto bytes = std::array<std::byte, write_size>{};
            bytes.fill(static_cast<std::byte>(index));
            writer.write_bytes(bytes);
            sync.arrive_and_wait();
            sync.arrive_and_wait();
            writer.commit();
            sync.arrive_and_wait();
        }
    }};

    auto valid = true;
    for (auto index = 0; index < write_count; ++index) {
        sync.arrive_and_wait();
        valid &= reader.readable().empty();
        sync.arrive_and_wait();
        sync.arrive_and_wait();

        auto remaining = std::size_t{write_size};
        while (remaining != 0) {
            const auto available = reader.readable();
            if (available.empty()) {
                valid = false;
                break;
            }
            const auto count = std::min(remaining, available.size());
            for (const auto byte : available.first(count)) {
                valid &= byte == static_cast<std::byte>(index);
            }
            reader.consume(count);
            remaining -= count;
        }
    }
    producer.join();
    CHECK(valid);
}

TEST_CASE("an unsized typed write moves whole objects to the next block", "[coverage-exempt]") {
    struct value {
        explicit value(std::uint64_t data) : data{data} {}
        std::uint64_t data;
    };

    auto data = bg::util::buffer::chain(1);
    bg::util::buffer_writer writer{data};
    writer.write(std::byte{'x'});
    writer.emplace<value>(42);
    writer.commit();

    bg::util::buffer_reader reader{data};
    CHECK(reader.read<std::byte>() == std::byte{'x'});
    CHECK(reader.read<value>().data == 42);
}

TEST_CASE("a single writer alternates unsized writes and sized reservations", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(2);
    bg::util::buffer_writer writer{data};
    constexpr auto first = std::string_view{"abc"};
    writer.write_bytes(std::as_bytes(std::span{first.data(), first.size()}));
    writer.commit();

    auto reservation = writer.reserve(1);
    reservation.write_bytes(std::as_bytes(std::span{"d", 1}));
    reservation.commit();

    constexpr auto last = std::string_view{"e"};
    writer.write_bytes(std::as_bytes(std::span{last.data(), last.size()}));
    writer.commit();

    bg::util::buffer_reader reader{data};
    for (const auto part : {std::string_view{"ab"}, std::string_view{"cd"}, std::string_view{"e"}}) {
        const auto available = reader.readable();
        REQUIRE(available.size() == part.size());
        CHECK(std::memcmp(available.data(), part.data(), part.size()) == 0);
        reader.consume(part.size());
    }
}

TEST_CASE("a failed unsized typed construction can be retried", "[coverage-exempt]") {
    struct value {
        explicit value(bool fail) {
            if (fail) {
                throw std::runtime_error{"requested construction failure"};
            }
        }
        std::uint32_t data{42};
    };

    auto data = bg::util::buffer::chain(1);
    bg::util::buffer_writer writer{data};
    writer.write(std::byte{'x'});
    CHECK_THROWS_AS(writer.emplace<value>(true), std::runtime_error);
    writer.emplace<value>(false);
    writer.commit();

    bg::util::buffer_reader reader{data};
    CHECK(reader.read<std::byte>() == std::byte{'x'});
    CHECK(reader.read<value>().data == 42);
}

TEST_CASE("reservations reject a second commit", "[coverage-exempt]") {
    {
        auto data = bg::util::buffer::contiguous(4);
        bg::util::buffer_writer writer{data};
        auto reservation = writer.reserve(1);
        reservation.commit();
        CHECK_THROWS_AS(reservation.commit(), std::logic_error);
    }

    {
        auto data = bg::util::buffer::concurrent_chain(8, 4);
        bg::util::concurrent_buffer_writer writer{data};
        auto reservation = writer.reserve(1);
        reservation.commit();
        CHECK_THROWS_AS(reservation.commit(), std::logic_error);
    }
}

TEST_CASE("a reader reads a contiguous buffer", "[coverage-exempt]") {
    auto data = bg::util::buffer::contiguous(8);
    bg::util::buffer_writer writer{data};
    auto reservation = writer.reserve(3);
    std::memcpy(reservation.bytes().data(), "abc", 3);
    reservation.commit();

    bg::util::buffer_reader reader{std::move(data)};
    CHECK(std::memcmp(reader.readable().data(), "abc", 3) == 0);
}

TEST_CASE("typed access skips reservation alignment padding", "[coverage-exempt]") {
    struct alignas(16) value {
        explicit value(std::uint64_t data) : data{data} {}
        std::uint64_t data;
    };

    auto data = bg::util::buffer::contiguous(128);
    bg::util::buffer_writer writer{data};
    const std::array marker{std::byte{'x'}};
    auto raw = writer.reserve(marker.size());
    raw.write_bytes(marker);
    raw.commit();

    auto typed = writer.reserve(sizeof(value), alignof(value));
    REQUIRE(typed.bytes().size() == sizeof(value));
    auto &written = typed.emplace<value>(42);
    CHECK(reinterpret_cast<std::uintptr_t>(&written) % alignof(value) == 0);
    typed.commit();
    CHECK(typed.bytes().empty());

    bg::util::buffer_reader reader{std::move(data)};
    CHECK(reader.read<std::byte>() == marker[0]);
    CHECK(reader.peek<value>().data == 42);
    CHECK(reader.read<value>().data == 42);
}

TEST_CASE("a reservation cursor writes raw bytes and typed lvalues and rvalues", "[coverage-exempt]") {
    struct value {
        explicit value(std::uint64_t data) : data{data} {}
        std::uint64_t data;
    };

    auto data = bg::util::buffer::contiguous(128);
    bg::util::buffer_writer writer{data};
    const std::array marker{std::byte{'x'}};
    auto reservation = writer.reserve(marker.size() + (alignof(value) - 1) + 2 * sizeof(value));
    reservation.write_bytes(marker);
    auto first = value{42};
    reservation.write(first);
    reservation.write(value{43});
    CHECK(reservation.bytes().size() == marker.size() + (alignof(value) - 1) + 2 * sizeof(value));
    reservation.commit();

    bg::util::buffer_reader reader{std::move(data)};
    CHECK(reader.read<std::byte>() == marker[0]);
    CHECK(reader.read<value>().data == 42);
    CHECK(reader.read<value>().data == 43);
}

TEST_CASE("a failed typed construction leaves the reservation cursor unchanged", "[coverage-exempt]") {
    struct value {
        value(std::uint32_t data, bool fail) : data{data} {
            if (fail) {
                throw std::runtime_error{"requested construction failure"};
            }
        }
        std::uint32_t data;
    };

    auto data = bg::util::buffer::contiguous(sizeof(value));
    bg::util::buffer_writer writer{data};
    auto reservation = writer.reserve(sizeof(value), alignof(value));
    CHECK_THROWS_AS(reservation.emplace<value>(1, true), std::runtime_error);
    CHECK(reservation.emplace<value>(2, false).data == 2);
    reservation.commit();

    bg::util::buffer_reader reader{std::move(data)};
    CHECK(reader.read<value>().data == 2);
}

TEST_CASE("typed reads and skips destroy nontrivial values", "[coverage-exempt]") {
    struct value {
        explicit value(int &destructions) : destructions{&destructions} {}
        value(value &&other) noexcept : destructions{other.destructions} {}
        ~value() {
            if (destructions != nullptr) {
                ++*destructions;
            }
        }
        int *destructions;
    };

    auto destructions = 0;
    {
        auto data = bg::util::buffer::contiguous(2 * sizeof(value));
        bg::util::buffer_writer writer{data};
        auto reservation = writer.reserve(2 * sizeof(value), alignof(value));
        reservation.emplace<value>(destructions);
        reservation.emplace<value>(destructions);
        reservation.commit();

        bg::util::buffer_reader reader{std::move(data)};
        auto first = reader.read<value>();
        CHECK(destructions == 1);
        reader.skip<value>();
        CHECK(destructions == 2);
    }
    CHECK(destructions == 3);
}

TEST_CASE("aligned typed reservations roll over single-writer and concurrent blocks", "[coverage-exempt]") {
    struct alignas(16) value {
        explicit value(std::uint64_t data) : data{data} {}
        std::uint64_t data;
    };

    {
        auto data = bg::util::buffer::chain(4);
        bg::util::buffer_writer writer{data};
        auto raw = writer.reserve(4);
        raw.bytes()[0] = std::byte{'a'};
        raw.commit();
        auto typed = writer.reserve(sizeof(value), alignof(value));
        typed.emplace<value>(42);
        typed.commit();

        bg::util::buffer_reader reader{std::move(data)};
        REQUIRE(reader.readable().size() == 4);
        reader.consume(4);
        CHECK(reader.read<value>().data == 42);
    }

    {
        auto data = bg::util::buffer::concurrent_chain(4, 1);
        bg::util::concurrent_buffer_writer writer{data};
        auto raw = writer.reserve(4);
        raw.bytes()[0] = std::byte{'a'};
        raw.commit();
        auto typed = writer.reserve(sizeof(value), alignof(value));
        CHECK(reinterpret_cast<std::uintptr_t>(typed.bytes().data()) % alignof(value) == 0);
        typed.emplace<value>(42);
        typed.commit();

        bg::util::buffer_reader reader{std::move(data)};
        REQUIRE(reader.readable().size() == 4);
        reader.consume(4);
        CHECK(reader.read<value>().data == 42);
    }
}

TEST_CASE("contiguous and single-writer reservations allocate no per-reservation state", "[coverage-exempt]") {
    auto memory = counting_memory_resource{};
    const auto exercise = [&](bg::util::buffer data) {
        REQUIRE(memory.allocations() == 1);
        bg::util::buffer_writer writer{std::move(data)};
        for (auto index = 0; index < 8; ++index) {
            auto reservation = writer.reserve(1);
            reservation.bytes()[0] = static_cast<std::byte>(index);
            reservation.commit();
        }
        CHECK(memory.allocations() == 1);
    };

    exercise(bg::util::buffer::contiguous(32, bg::allocator{memory}));
    CHECK(memory.allocations() == 0);
    exercise(bg::util::buffer::chain(32, bg::allocator{memory}));
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a single writer rolls over without exposing unused tail bytes", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(4);
    bg::util::buffer_writer writer{data};
    auto first = writer.reserve(3);
    std::memcpy(first.bytes().data(), "abc", 3);
    first.commit();

    auto second = writer.reserve(3);
    std::memcpy(second.bytes().data(), "def", 3);
    second.commit();

    bg::util::buffer_reader reader{data};
    auto bytes = reader.readable();
    CHECK(bytes.size() == 3);
    CHECK(std::memcmp(bytes.data(), "abc", 3) == 0);
    reader.consume(3);

    bytes = reader.readable();
    CHECK(bytes.size() == 3);
    CHECK(std::memcmp(bytes.data(), "def", 3) == 0);
}

TEST_CASE("an oversized block rolls back to the preferred capacity", "[coverage-exempt]") {
    auto memory = counting_memory_resource{};
    {
        auto data = bg::util::buffer::chain(3, bg::allocator{memory});
        bg::util::buffer_writer writer{data};

        auto first = writer.reserve(3);
        first.commit();
        auto oversized = writer.reserve(8);
        oversized.commit();
        auto preferred = writer.reserve(1);
        preferred.commit();

        constexpr auto overhead = sizeof(bg::util::details::single_writer_block);
        CHECK(memory.allocations() == 3);
        CHECK(memory.allocated_bytes() == 3 * overhead + 3 + 8 + 3);
    }
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a writer resumed from an old handle releases consumed blocks", "[coverage-exempt]") {
    auto memory = counting_memory_resource{};
    {
        auto data = bg::util::buffer::chain(3, bg::allocator{memory});
        {
            bg::util::buffer_writer writer{data};
            auto first = writer.reserve(3);
            first.commit();
            auto second = writer.reserve(2);
            second.commit();
        }

        REQUIRE(memory.allocations() == 2);
        {
            bg::util::buffer_writer resumed{std::move(data)};
            CHECK(memory.allocations() == 1);
            auto next = resumed.reserve(1);
            next.commit();
        }
        CHECK(memory.allocations() == 0);
    }
}

TEST_CASE("reader exposes only the committed prefix while an earlier reservation is pending", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(8, 4);
    bg::util::concurrent_buffer_writer first_writer{data};
    bg::util::concurrent_buffer_writer pending_writer{data};
    bg::util::concurrent_buffer_writer later_writer{data};
    auto first = first_writer.reserve(3);
    auto pending = pending_writer.reserve(2);
    auto later = later_writer.reserve(2);
    std::memcpy(first.bytes().data(), "abc", 3);
    std::memcpy(pending.bytes().data(), "DE", 2);
    std::memcpy(later.bytes().data(), "fg", 2);

    bg::util::concurrent_buffer_reader reader{data};
    first.commit();
    later.commit();
    auto bytes = reader.readable();
    REQUIRE(bytes.size() == 3);
    CHECK(std::memcmp(bytes.data(), "abc", 3) == 0);

    pending.commit();
    reader.consume(3);
    bytes = reader.readable();
    REQUIRE(bytes.size() == 4);
    CHECK(std::memcmp(bytes.data(), "DEfg", 4) == 0);
}

TEST_CASE("concurrent rollover waits for earlier commits before exposing the next block", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(4, 4);
    bg::util::concurrent_buffer_writer first_writer{data};
    bg::util::concurrent_buffer_writer second_writer{data};
    auto first = first_writer.reserve(3);
    auto second = second_writer.reserve(2);
    std::memcpy(first.bytes().data(), "abc", 3);
    std::memcpy(second.bytes().data(), "DE", 2);

    bg::util::concurrent_buffer_reader reader{data};
    second.commit();
    CHECK(reader.readable().empty());

    first.commit();
    auto bytes = reader.readable();
    CHECK(bytes.size() == 3);
    CHECK(std::memcmp(bytes.data(), "abc", 3) == 0);
    reader.consume(3);

    bytes = reader.readable();
    CHECK(bytes.size() == 2);
    CHECK(std::memcmp(bytes.data(), "DE", 2) == 0);
}

TEST_CASE("a reader block starts a writer after the reader starts", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(3, 4);
    bg::util::concurrent_buffer_reader reader{data};

    CHECK(reader.current_block().type() == bg::util::buffer_type::concurrent_writer_chain);

    bg::util::concurrent_buffer_writer writer{reader.current_block()};
    auto first = writer.reserve(3);
    std::memcpy(first.bytes().data(), "abc", 3);
    first.commit();
    auto second = writer.reserve(2);
    std::memcpy(second.bytes().data(), "de", 2);
    second.commit();

    auto bytes = reader.readable();
    REQUIRE(bytes.size() == 3);
    CHECK(std::memcmp(bytes.data(), "abc", 3) == 0);
    reader.consume(3);
    bytes = reader.readable();
    REQUIRE(bytes.size() == 2);
    CHECK(std::memcmp(bytes.data(), "de", 2) == 0);
}

TEST_CASE("a contiguous chunk owns its allocation and reports the contiguous type", "[coverage-exempt]") {
    std::optional<bg::util::buffer> chunk;
    {
        auto data = bg::util::buffer::contiguous(8);
        bg::util::buffer_writer writer{data};
        auto reservation = writer.reserve(5);
        std::memcpy(reservation.bytes().data(), "hello", 5);
        reservation.commit();
        chunk = data.chunk(1, 4);
    }

    REQUIRE(chunk.has_value());
    CHECK(chunk->type() == bg::util::buffer_type::contiguous);
    CHECK(chunk->size() == 3);
    CHECK(std::memcmp(chunk->bytes().data(), "ell", 3) == 0);
}

TEST_CASE("a chunk is not writable through its backing allocation", "[coverage-exempt]") {
    auto data = bg::util::buffer::contiguous(8);
    bg::util::buffer_writer writer{data};
    auto reservation = writer.reserve(4);
    std::memcpy(reservation.bytes().data(), "data", 4);
    reservation.commit();

    auto chunk = data.chunk(1, 3);
    CHECK_THROWS_AS(bg::util::buffer_writer{chunk}, std::invalid_argument);
}

TEST_CASE("a full block commit ring rolls over while an earlier reservation is pending", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(8, 1);
    bg::util::concurrent_buffer_writer first_writer{data};
    bg::util::concurrent_buffer_writer second_writer{data};
    auto first = first_writer.reserve(1);
    first.bytes()[0] = std::byte{'a'};
    auto second = second_writer.reserve(1);
    second.bytes()[0] = std::byte{'b'};
    second.commit();

    bg::util::concurrent_buffer_reader reader{data};
    CHECK(reader.readable().empty());

    first.commit();
    auto bytes = reader.readable();
    REQUIRE(bytes.size() == 1);
    CHECK(bytes[0] == std::byte{'a'});
    reader.consume(1);
    bytes = reader.readable();
    REQUIRE(bytes.size() == 1);
    CHECK(bytes[0] == std::byte{'b'});
}

TEST_CASE("a concurrent block rolls over before its local sequence wraps", "[coverage-exempt]") {
    using namespace bg::util::details;

    auto *block = new_concurrent_writer_block(bg::allocator{}, 8, 2);
    auto block_owner = block_reference::adopt(block);
    auto &control = block->control;
    const auto previous_sequence = concurrent_writer_block::maximum_sequence - 1;
    auto mark = block->make_reservation_mark(0, previous_sequence);
    control.reservation_mark.store(mark, std::memory_order_relaxed);
    control.published.store(previous_sequence, std::memory_order_relaxed);

    CHECK(control.reservation_mark.compare_exchange_strong(
        mark, block->make_reservation_mark(1, concurrent_writer_block::maximum_sequence), std::memory_order_acq_rel));
    block->at(block->data_offset())[0] = std::byte{'a'};
    block->commit(previous_sequence, 1);

    const auto last_mark = control.reservation_mark.load(std::memory_order_acquire);
    CHECK(control.published.load(std::memory_order_acquire) == concurrent_writer_block::maximum_sequence);
    CHECK(block->committed.load(std::memory_order_acquire) == block->data_offset() + 1);
    CHECK(block->needs_rollover(last_mark, 1, 1));
    auto *next_block = block->next_for_reservation(1, 1);
    REQUIRE(next_block != nullptr);
    REQUIRE(next_block != block);
    CHECK(concurrent_writer_block::mark_is_sealed(control.reservation_mark.load(std::memory_order_acquire)));
    CHECK(concurrent_writer_block::mark_sequence(control.reservation_mark.load(std::memory_order_acquire)) ==
          concurrent_writer_block::maximum_sequence);
    CHECK(block->next.load(std::memory_order_acquire) == next_block);
    CHECK(concurrent_writer_block::mark_position(
              next_block->control.reservation_mark.load(std::memory_order_relaxed)) == 0);
    CHECK(concurrent_writer_block::mark_sequence(
              next_block->control.reservation_mark.load(std::memory_order_relaxed)) == 0);
}

TEST_CASE("a concurrent chain can retry rollover after allocation failure", "[coverage-exempt]") {
    auto memory = counting_memory_resource{};
    {
        auto allocator = bg::allocator{memory};
        auto data = bg::util::buffer::concurrent_chain(2, 2, allocator);
        const auto initial_allocations = memory.allocations();
        const auto initial_bytes = memory.allocated_bytes();
        CHECK(initial_allocations == 1);
        bg::util::concurrent_buffer_writer writer{data};
        auto first = writer.reserve(2);
        std::memcpy(first.bytes().data(), "ab", 2);
        first.commit();

        memory.fail_after(0);
        CHECK_THROWS_AS(writer.reserve(1), std::bad_alloc);
        CHECK(memory.allocations() == initial_allocations);

        memory.allow_allocations();
        auto second = writer.reserve(1);
        second.bytes()[0] = std::byte{'c'};
        second.commit();
        CHECK(memory.allocations() == initial_allocations + 1);
        CHECK(memory.allocated_bytes() ==
              initial_bytes + bg::util::details::concurrent_writer_block::data_offset(2) + 2);

        bg::util::concurrent_buffer_reader reader{data};
        auto bytes = reader.readable();
        REQUIRE(bytes.size() == 2);
        CHECK(std::memcmp(bytes.data(), "ab", 2) == 0);
        reader.consume(2);
        bytes = reader.readable();
        REQUIRE(bytes.size() == 1);
        CHECK(bytes[0] == std::byte{'c'});
    }
    CHECK(memory.allocations() == 0);
}

TEST_CASE("block-local commit order hides later blocks until earlier reservations commit", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(2, 2);
    bg::util::concurrent_buffer_writer first_writer{data};
    bg::util::concurrent_buffer_writer second_writer{data};
    bg::util::concurrent_buffer_writer third_writer{data};
    auto first = first_writer.reserve(2);
    auto second = second_writer.reserve(2);
    std::memcpy(first.bytes().data(), "ab", 2);
    std::memcpy(second.bytes().data(), "cd", 2);
    auto third = third_writer.reserve(2);
    std::memcpy(third.bytes().data(), "ef", 2);
    third.commit();

    bg::util::concurrent_buffer_reader reader{data};
    CHECK(reader.readable().empty());
    second.commit();
    CHECK(reader.readable().empty());
    first.commit();

    std::array<char, 6> actual{};
    std::size_t offset = 0;
    for (;;) {
        const auto bytes = reader.readable();
        if (bytes.empty()) {
            break;
        }
        std::memcpy(actual.data() + offset, bytes.data(), bytes.size());
        offset += bytes.size();
        reader.consume(bytes.size());
    }
    CHECK(std::string_view{actual.data(), actual.size()} == "abcdef");
}

TEST_CASE("a moved chain handle transfers ownership to its writer", "[coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto allocator = bg::allocator{memory};
    std::optional<bg::util::concurrent_buffer_writer> writer;
    std::optional<bg::util::concurrent_buffer_reservation> reservation;
    {
        auto data = bg::util::buffer::concurrent_chain(8, 2, allocator);
        writer.emplace(std::move(data));
        reservation.emplace(writer->reserve(3));
        std::memcpy(reservation->bytes().data(), "abc", 3);
    }

    REQUIRE(memory.allocations() == 1);
    reservation->commit();
    CHECK(reservation->bytes().empty());
    reservation.reset();
    writer.reset();
    CHECK(memory.allocations() == 0);
}

TEST_CASE("reader spans contain payload bytes only", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(16, 4);
    bg::util::concurrent_buffer_writer first_writer{data};
    bg::util::concurrent_buffer_writer second_writer{data};
    auto first = first_writer.reserve(2);
    auto second = second_writer.reserve(3);
    std::memcpy(first.bytes().data(), "ab", 2);
    std::memcpy(second.bytes().data(), "cde", 3);
    first.commit();
    second.commit();

    bg::util::concurrent_buffer_reader reader{data};
    const auto bytes = reader.readable();
    REQUIRE(bytes.size() == 5);
    CHECK(std::memcmp(bytes.data(), "abcde", 5) == 0);
}

TEST_CASE("a concurrent reservation reads its completed prefix for cleanup", "[coverage-exempt]") {
    struct owned_value {
        owned_value(std::unique_ptr<int> value, int &destructions)
            : value{std::move(value)}, destructions{&destructions} {}
        ~owned_value() { ++*destructions; }

        std::unique_ptr<int> value;
        int *destructions;
    };
    struct throwing_value {
        throwing_value() { throw std::runtime_error{"construction failed"}; }
    };

    auto data = bg::util::buffer::concurrent_chain(64, 4);
    bg::util::concurrent_buffer_writer writer{data};
    auto reservation = writer.reserve(64);
    auto destructions = 0;
    reservation.emplace<owned_value>(std::make_unique<int>(42), destructions);
    CHECK_THROWS_AS(reservation.emplace<throwing_value>(), std::runtime_error);

    reservation.read_written([&](bg::util::buffer_reader &reader) {
        CHECK_THROWS_AS(reader.fork(), std::logic_error);
        CHECK_THROWS_AS(reader.current_buffer(), std::logic_error);
        CHECK_THROWS_AS(reader.current_block(), std::logic_error);
        reader.skip<owned_value>();
        CHECK(reader.readable().empty());
    });

    CHECK(destructions == 1);
    reservation.commit();
}

TEST_CASE("a reservation reader excludes its tail and neighboring writes", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(32, 4);
    bg::util::concurrent_buffer_writer first_writer{data};
    bg::util::concurrent_buffer_writer second_writer{data};
    auto first = first_writer.reserve(8);
    auto second = second_writer.reserve(8);
    constexpr auto first_prefix = std::string_view{"abc"};
    constexpr auto second_prefix = std::string_view{"XYZ"};
    first.write_bytes(std::as_bytes(std::span{first_prefix.data(), first_prefix.size()}));
    second.write_bytes(std::as_bytes(std::span{second_prefix.data(), second_prefix.size()}));
    second.commit();

    bg::util::concurrent_buffer_reader reader{data};
    CHECK(reader.readable().empty());
    first.read_written([&](bg::util::buffer_reader &written) {
        auto bytes = written.readable();
        REQUIRE(bytes.size() == first_prefix.size());
        CHECK(std::memcmp(bytes.data(), first_prefix.data(), bytes.size()) == 0);
        written.consume(bytes.size());
        CHECK(written.readable().empty());
    });
    CHECK(reader.readable().empty());

    constexpr auto first_tail = std::string_view{"defgh"};
    first.write_bytes(std::as_bytes(std::span{first_tail.data(), first_tail.size()}));
    first.commit();

    std::array<char, 3> actual_first{};
    reader.read_bytes(std::as_writable_bytes(std::span{actual_first}));
    CHECK(std::string_view{actual_first.data(), actual_first.size()} == first_prefix);
    reader.consume(5);
    std::array<char, 3> actual_second{};
    reader.read_bytes(std::as_writable_bytes(std::span{actual_second}));
    CHECK(std::string_view{actual_second.data(), actual_second.size()} == second_prefix);
}

TEST_CASE("a reservation reader aligns typed values from the absolute block position", "[coverage-exempt]") {
    struct alignas(std::max_align_t) aligned_value {
        explicit aligned_value(int value) : value{value} {}
        int value;
    };

    auto data = bg::util::buffer::concurrent_chain(128, 4);
    bg::util::concurrent_buffer_writer writer{data};
    auto prefix = writer.reserve(1);
    const auto prefix_byte = std::array{std::byte{0x7f}};
    prefix.write_bytes(prefix_byte);
    prefix.commit();

    auto reservation = writer.reserve(64);
    reservation.emplace<aligned_value>(73);
    reservation.read_written([](bg::util::buffer_reader &reader) {
        CHECK(reader.read<aligned_value>().value == 73);
        CHECK(reader.readable().empty());
    });
    reservation.commit();
}

TEST_CASE("empty and failed reservation reads leave reservations available", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(32, 4);
    bg::util::concurrent_buffer_writer writer{data};
    auto empty = writer.reserve(8);
    empty.read_written([](bg::util::buffer_reader &reader) { CHECK(reader.readable().empty()); });
    empty.commit();

    auto failed = writer.reserve(8);
    const auto first = std::array{std::byte{0x11}};
    failed.write_bytes(first);
    CHECK_THROWS_AS(
        failed.read_written([](bg::util::buffer_reader &reader) { static_cast<void>(reader.read<std::uint64_t>()); }),
        std::out_of_range);
    const auto rest = std::array<std::byte, 7>{};
    failed.write_bytes(rest);
    failed.commit();
    CHECK_THROWS_AS(failed.read_written([](bg::util::buffer_reader &) {}), std::logic_error);

    auto moved_from = writer.reserve(8);
    auto active = std::move(moved_from);
    CHECK_THROWS_AS(moved_from.read_written([](bg::util::buffer_reader &) {}), std::logic_error);
    active.commit();
}

TEST_CASE("a concurrent chain preserves committed bytes across rollover contention", "[coverage-exempt]") {
    constexpr auto thread_count = 8;
    constexpr auto writes_per_thread = 500;
    auto data = bg::util::buffer::concurrent_chain(3, 8);
    std::vector<std::thread> writers;

    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        writers.emplace_back([&, thread] {
            bg::util::concurrent_buffer_writer writer{data};
            for (std::size_t write = 0; write < writes_per_thread; ++write) {
                auto reservation = writer.reserve(2);
                reservation.bytes()[0] = static_cast<std::byte>(thread);
                reservation.bytes()[1] = std::byte{0x7f};
                reservation.commit();
            }
        });
    }

    for (auto &thread : writers) {
        thread.join();
    }

    bg::util::concurrent_buffer_reader reader{data};
    std::array<std::size_t, thread_count> counts{};
    std::size_t total = 0;
    for (;;) {
        const auto bytes = reader.readable();
        if (bytes.empty()) {
            break;
        }
        REQUIRE(bytes.size() % 2 == 0);
        for (std::size_t offset = 0; offset < bytes.size(); offset += 2) {
            const auto source = std::to_integer<std::size_t>(bytes[offset]);
            REQUIRE(source < thread_count);
            CHECK(bytes[offset + 1] == std::byte{0x7f});
            ++counts[source];
            total += 2;
        }
        reader.consume(bytes.size());
    }

    CHECK(total == thread_count * writes_per_thread * 2);
    for (const auto count : counts) {
        CHECK(count == writes_per_thread);
    }
}

TEST_CASE("a concurrent reader drains records during rollover and retained chunks outlive the root",
          "[coverage-exempt]") {
    constexpr auto writer_count = std::size_t{4};
    constexpr auto writes_per_writer = std::size_t{400};
    using record = std::array<std::uint32_t, 3>;
    constexpr auto record_size = sizeof(record);
    const auto checksum = [](std::uint32_t writer, std::uint32_t sequence) {
        return (writer + 1) * 0x9e3779b9u ^ sequence ^ 0x85ebca6bu;
    };

    std::optional<bg::util::buffer> retained_chunk;
    record retained_value{};
    {
        auto data = bg::util::buffer::concurrent_chain(record_size * 4, 8);
        {
            bg::util::concurrent_buffer_reader reader{data};
            auto start_block = reader.current_block();
            auto start = std::barrier{static_cast<std::ptrdiff_t>(writer_count + 1)};
            auto writers_finished = std::atomic<std::size_t>{};
            std::vector<std::thread> writers;

            for (std::size_t id = 0; id < writer_count; ++id) {
                writers.emplace_back([&, id] {
                    bg::util::concurrent_buffer_writer writer{start_block};
                    start.arrive_and_wait();
                    for (std::size_t sequence = 0; sequence < writes_per_writer; ++sequence) {
                        const auto value =
                            record{static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(sequence),
                                   checksum(static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(sequence))};
                        auto reservation = writer.reserve(record_size);
                        std::memcpy(reservation.bytes().data(), &value, record_size);
                        reservation.commit();
                    }
                    writers_finished.fetch_add(1, std::memory_order_release);
                });
            }

            start.arrive_and_wait();
            std::array<std::uint32_t, writer_count> next_sequence{};
            auto records_read = std::size_t{};
            auto records_valid = true;

            while (records_read < writer_count * writes_per_writer) {
                const auto bytes = reader.readable();
                if (bytes.empty()) {
                    if (writers_finished.load(std::memory_order_acquire) == writer_count) {
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }

                if (!retained_chunk) {
                    auto current = reader.current_buffer();
                    retained_chunk = current.chunk(0, record_size);
                    std::memcpy(&retained_value, retained_chunk->bytes().data(), record_size);
                }

                records_valid &= bytes.size() % record_size == 0;
                for (auto offset = std::size_t{}; offset + record_size <= bytes.size(); offset += record_size) {
                    auto value = record{};
                    std::memcpy(&value, bytes.data() + offset, record_size);
                    if (value[0] >= writer_count) {
                        records_valid = false;
                        continue;
                    }

                    records_valid &= value[1] == next_sequence[value[0]];
                    records_valid &= value[2] == checksum(value[0], value[1]);
                    ++next_sequence[value[0]];
                    ++records_read;
                }
                reader.consume(bytes.size());
            }

            for (auto &thread : writers) {
                thread.join();
            }

            CHECK(records_valid);
            CHECK(records_read == writer_count * writes_per_writer);
            for (const auto sequence : next_sequence) {
                CHECK(sequence == writes_per_writer);
            }
        }
    }

    REQUIRE(retained_chunk.has_value());
    CHECK(retained_chunk->size() == record_size);
    CHECK(std::memcmp(retained_chunk->bytes().data(), &retained_value, record_size) == 0);
}

TEST_CASE("barrier-started publishers preserve the prefix across repeated ring reuse", "[coverage-exempt]") {
    constexpr auto writer_count = std::size_t{8};
    constexpr auto batches = std::size_t{128};
    using record = std::array<std::uint32_t, 3>;
    constexpr auto record_size = sizeof(record);
    const auto checksum = [](std::uint32_t writer, std::uint32_t batch) {
        return (writer + 1) * 0x9e3779b9u ^ batch ^ 0x85ebca6bu;
    };

    auto data = bg::util::buffer::concurrent_chain(record_size * writer_count, writer_count);
    std::array<std::unique_ptr<bg::util::concurrent_buffer_writer>, writer_count> writers;
    for (auto &writer : writers) {
        writer = std::make_unique<bg::util::concurrent_buffer_writer>(data);
    }
    std::array<std::optional<bg::util::concurrent_buffer_reservation>, writer_count> reservations;
    auto start = std::barrier{static_cast<std::ptrdiff_t>(writer_count + 1)};
    auto finished = std::barrier{static_cast<std::ptrdiff_t>(writer_count + 1)};
    std::vector<std::thread> committers;

    for (std::size_t id = 0; id < writer_count; ++id) {
        committers.emplace_back([&, id] {
            for (std::size_t batch = 0; batch < batches; ++batch) {
                start.arrive_and_wait();
                reservations[id]->commit();
                finished.arrive_and_wait();
            }
        });
    }

    for (std::size_t batch = 0; batch < batches; ++batch) {
        for (std::size_t id = 0; id < writer_count; ++id) {
            reservations[id].emplace(writers[id]->reserve(record_size));
            const auto value = record{static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(batch),
                                      checksum(static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(batch))};
            std::memcpy(reservations[id]->bytes().data(), &value, record_size);
        }

        start.arrive_and_wait();
        finished.arrive_and_wait();
    }

    for (auto &thread : committers) {
        thread.join();
    }

    bg::util::concurrent_buffer_reader reader{data};
    auto records_read = std::size_t{};
    auto records_valid = true;
    for (;;) {
        const auto bytes = reader.readable();
        if (bytes.empty()) {
            break;
        }

        records_valid &= bytes.size() % record_size == 0;
        for (auto offset = std::size_t{}; offset + record_size <= bytes.size(); offset += record_size) {
            auto value = record{};
            std::memcpy(&value, bytes.data() + offset, record_size);
            const auto expected_batch = records_read / writer_count;
            const auto expected_writer = records_read % writer_count;
            records_valid &= value[0] == expected_writer;
            records_valid &= value[1] == expected_batch;
            records_valid &= value[2] == checksum(value[0], value[1]);
            ++records_read;
        }
        reader.consume(bytes.size());
    }

    CHECK(records_valid);
    CHECK(records_read == batches * writer_count);
}

TEST_CASE("a chain does not expose a misleading contiguous range", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(4);

    CHECK_THROWS_AS(data.size(), std::logic_error);
    CHECK_THROWS_AS(data.bytes(), std::logic_error);
    CHECK_THROWS_AS(data.chunk(0, 0), std::logic_error);
}

TEST_CASE("a reader's current buffer remains valid after advancing the chain", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(3);
    bg::util::buffer_writer writer{data};
    auto first = writer.reserve(3);
    std::memcpy(first.bytes().data(), "abc", 3);
    first.commit();
    auto second = writer.reserve(2);
    std::memcpy(second.bytes().data(), "de", 2);
    second.commit();

    bg::util::buffer_reader reader{data};
    auto current = reader.current_buffer();
    auto chunk = current.chunk(1, 3);
    reader.consume(3);
    CHECK(std::memcmp(reader.readable().data(), "de", 2) == 0);
    CHECK(std::memcmp(chunk.bytes().data(), "bc", 2) == 0);
}

TEST_CASE("a restarted reader resumes and retained chunks keep only their block", "[coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto allocator = bg::allocator{memory};
    {
        auto data = bg::util::buffer::chain(3, allocator);
        bg::util::buffer_writer writer{data};
        auto first = writer.reserve(3);
        std::memcpy(first.bytes().data(), "abc", 3);
        first.commit();
        auto second = writer.reserve(2);
        std::memcpy(second.bytes().data(), "de", 2);
        second.commit();
        const auto allocations_with_two_blocks = memory.allocations();

        std::optional<bg::util::buffer> retained_chunk;
        {
            bg::util::buffer_reader reader{std::move(data)};
            auto first_block = reader.current_buffer();
            retained_chunk = first_block.chunk(0, 3);
            reader.consume(1);
            auto cursor = reader.current_block();

            bg::util::buffer_reader resumed{std::move(cursor)};
            CHECK(std::memcmp(resumed.readable().data(), "bc", 2) == 0);
            resumed.consume(2);
            CHECK(std::memcmp(resumed.readable().data(), "de", 2) == 0);
        }
        CHECK(memory.allocations() == allocations_with_two_blocks);

        retained_chunk.reset();
        CHECK(memory.allocations() + 1 == allocations_with_two_blocks);
    }

    CHECK(memory.allocations() == 0);
}

TEST_CASE("a single-writer chain permits independent reader handles", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(4);
    bg::util::buffer_writer writer{data};
    bg::util::buffer_reader first{data};
    bg::util::buffer_reader second{data};
    CHECK(first.readable().empty());
    CHECK(second.readable().empty());
}

TEST_CASE("a concurrent buffer requires a power-of-two commit capacity", "[coverage-exempt]") {
    CHECK_THROWS_AS(bg::util::buffer::concurrent_chain(8, 3), std::invalid_argument);
}
