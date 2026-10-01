#include <bg/util/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include "counting_memory_resource.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <future>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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

TEST_CASE("reader exposes only the committed prefix while an earlier reservation is pending", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(8, 4);
    bg::util::concurrent_buffer_writer writer{data};
    auto first = writer.reserve(3);
    auto pending = writer.reserve(2);
    auto later = writer.reserve(2);
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
    bg::util::concurrent_buffer_writer writer{data};
    auto first = writer.reserve(3);
    auto second = writer.reserve(2);
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

TEST_CASE("a full commit ring applies backpressure until the next commit advances", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(8, 1);
    bg::util::concurrent_buffer_writer writer{data};
    auto first = writer.reserve(1);
    first.bytes()[0] = std::byte{'a'};
    auto waiting_started = std::atomic<bool>{};

    auto waiting = std::async(std::launch::async, [&] {
        waiting_started.store(true, std::memory_order_release);
        auto second = writer.reserve(1);
        second.bytes()[0] = std::byte{'b'};
        second.commit();
    });
    while (!waiting_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK(waiting.wait_for(30ms) == std::future_status::timeout);

    first.commit();
    CHECK(waiting.wait_for(1s) == std::future_status::ready);
    waiting.get();

    bg::util::concurrent_buffer_reader reader{data};
    const auto bytes = reader.readable();
    REQUIRE(bytes.size() == 2);
    CHECK(bytes[0] == std::byte{'a'});
    CHECK(bytes[1] == std::byte{'b'});
}

TEST_CASE("a concurrent chain can retry rollover after allocation failure", "[coverage-exempt]") {
    auto memory = counting_memory_resource{};
    {
        auto allocator = bg::allocator{memory};
        auto data = bg::util::buffer::concurrent_chain(2, 2, allocator);
        const auto initial_allocations = memory.allocations();
        bg::util::concurrent_buffer_writer writer{data};
        auto first = writer.reserve(2);
        std::memcpy(first.bytes().data(), "ab", 2);
        first.commit();

        for (const auto successful_allocations : {0, 1}) {
            memory.fail_after(successful_allocations);
            CHECK_THROWS_AS(writer.reserve(1), std::bad_alloc);
            CHECK(memory.allocations() == initial_allocations);
        }

        memory.allow_allocations();
        auto second = writer.reserve(1);
        second.bytes()[0] = std::byte{'c'};
        second.commit();
        CHECK(memory.allocations() == initial_allocations + 1);

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

TEST_CASE("ring backpressure bounds reservations across rollover", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(2, 2);
    bg::util::concurrent_buffer_writer writer{data};
    auto first = writer.reserve(2);
    auto second = writer.reserve(2);
    std::memcpy(first.bytes().data(), "ab", 2);
    std::memcpy(second.bytes().data(), "cd", 2);
    auto waiting_started = std::atomic<bool>{};

    auto waiting = std::async(std::launch::async, [&] {
        waiting_started.store(true, std::memory_order_release);
        auto third = writer.reserve(2);
        std::memcpy(third.bytes().data(), "ef", 2);
        third.commit();
    });
    while (!waiting_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK(waiting.wait_for(30ms) == std::future_status::timeout);

    first.commit();
    CHECK(waiting.wait_for(1s) == std::future_status::ready);
    second.commit();
    waiting.get();

    bg::util::concurrent_buffer_reader reader{data};
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

TEST_CASE("reader spans contain payload bytes only", "[coverage-exempt]") {
    auto data = bg::util::buffer::concurrent_chain(16, 4);
    bg::util::concurrent_buffer_writer writer{data};
    auto first = writer.reserve(2);
    auto second = writer.reserve(3);
    std::memcpy(first.bytes().data(), "ab", 2);
    std::memcpy(second.bytes().data(), "cde", 3);
    first.commit();
    second.commit();

    bg::util::concurrent_buffer_reader reader{data};
    const auto bytes = reader.readable();
    REQUIRE(bytes.size() == 5);
    CHECK(std::memcmp(bytes.data(), "abcde", 5) == 0);
}

TEST_CASE("a concurrent chain preserves committed bytes across rollover contention", "[coverage-exempt]") {
    constexpr auto thread_count = 8;
    constexpr auto writes_per_thread = 500;
    auto data = bg::util::buffer::concurrent_chain(3, 8);
    bg::util::concurrent_buffer_writer writer{data};
    std::vector<std::thread> writers;

    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        writers.emplace_back([&, thread] {
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
            bg::util::buffer_reader reader{data};
            auto first_block = reader.current_buffer();
            retained_chunk = first_block.chunk(0, 3);
            reader.consume(1);
        }

        {
            bg::util::buffer_reader reader{data};
            CHECK(std::memcmp(reader.readable().data(), "bc", 2) == 0);
            reader.consume(2);
            CHECK(std::memcmp(reader.readable().data(), "de", 2) == 0);
            CHECK(memory.allocations() == allocations_with_two_blocks);
        }

        retained_chunk.reset();
        CHECK(memory.allocations() + 1 == allocations_with_two_blocks);
    }

    CHECK(memory.allocations() == 0);
}

TEST_CASE("a buffer rejects a second single reader or writer", "[coverage-exempt]") {
    auto data = bg::util::buffer::chain(4);
    bg::util::buffer_writer writer{data};
    CHECK_THROWS_AS(bg::util::buffer_writer{data}, std::logic_error);

    bg::util::buffer_reader reader{data};
    CHECK_THROWS_AS(bg::util::buffer_reader{data}, std::logic_error);
}

TEST_CASE("a concurrent buffer requires a power-of-two commit capacity", "[coverage-exempt]") {
    CHECK_THROWS_AS(bg::util::buffer::concurrent_chain(8, 3), std::invalid_argument);
}
