#include <bg/literal/time_series_literal.hpp>
#include <bg/time_series/output_state.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/util/buffer.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>

using namespace bg;

TEST_CASE("a queue message reader advances no-op frames and discards typed payloads",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto buffer = util::buffer::concurrent_chain(128, 4);
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    auto invalidation_writer = details::QueueMessageWriter{buffer, 70, runtime_type};
    auto value_writer = details::QueueMessageWriter{buffer, 71, runtime_type};
    REQUIRE(invalidation_writer.write(literal::TimeSeriesLiteral(literal::INVALID)));
    REQUIRE(value_writer.write(literal::TimeSeriesLiteral(9)));

    auto evaluation_time = MIN_DT + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{};
    auto value = i64{};
    auto output = OutputState{runtime_type};
    output.bind(last_updated, &value, evaluation_time);
    auto reader = details::QueueMessageReader{buffer};

    REQUIRE(reader.next_target() == 70);
    REQUIRE(reader.apply(output, QueueMode::standard));
    CHECK_FALSE(last_updated.valid());
    REQUIRE(reader.next_target() == 71);
    REQUIRE(reader.discard(TS<i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("a queue message reader skips sentinels across padded block frames",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto buffer = util::buffer::concurrent_chain(32, 4);
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();

    for (auto target = std::size_t{}; target < 12; ++target) {
        auto writer = details::QueueMessageWriter{buffer, target, runtime_type};
        REQUIRE(writer.write(literal::TimeSeriesLiteral(static_cast<i64>(target))));
    }

    auto mutator = util::buffer_reader{buffer};
    auto &first_header = const_cast<details::QueueMessageHeader &>(mutator.peek<details::QueueMessageHeader>());
    first_header.target = std::numeric_limits<std::uint32_t>::max();

    auto reader = details::QueueMessageReader{buffer};
    CHECK(reader.has_pending_target());
    for (auto target = std::uint32_t{1}; target < 12; ++target) {
        REQUIRE(reader.next_target() == target);
        REQUIRE(reader.discard(TS<i64>::type()));
    }
    CHECK_FALSE(reader.has_pending_target());
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("a queue message reader retains conflicts and batch rejection for retry",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto buffer = util::buffer::concurrent_chain(128, 4);
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    auto writer = details::QueueMessageWriter{buffer, 63, runtime_type};
    REQUIRE(writer.write(literal::TimeSeriesLiteral(42)));

    auto evaluation_time = MIN_DT + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{evaluation_time};
    auto value = i64{};
    auto output = OutputState{runtime_type};
    output.bind(last_updated, &value, evaluation_time);
    auto reader = details::QueueMessageReader{buffer};

    REQUIRE(reader.next_target() == 63);
    CHECK_THROWS_AS(reader.apply(output, QueueMode::batch), std::invalid_argument);
    CHECK(reader.next_target() == 63);
    CHECK_FALSE(reader.apply(output, QueueMode::standard));
    CHECK(reader.next_target() == 63);

    evaluation_time += std::chrono::microseconds{1};
    REQUIRE(reader.apply(output, QueueMode::standard));
    CHECK(value == 42);
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("a queue message reader respects an unfinished earlier reservation",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    constexpr auto frame_size = std::size_t{32};
    const auto buffer = util::buffer::concurrent_chain(64, 1);
    auto first_writer = util::concurrent_buffer_writer{buffer};
    auto second_writer = util::concurrent_buffer_writer{buffer};
    auto first = first_writer.reserve(frame_size, alignof(details::QueueMessageHeader));
    auto second = second_writer.reserve(frame_size, alignof(details::QueueMessageHeader));
    const auto write_frame = [&](auto &reservation, std::uint32_t target, i64 value) {
        auto context = ScalarBufferWriteContext{reservation};
        context.write(details::QueueMessageHeader{target, static_cast<std::uint32_t>(frame_size)});
        details::TickHeader{details::TickOperation::tick, true}.write(context);
        context.write_bytes(std::as_bytes(std::span{&value, std::size_t{1}}));
    };
    write_frame(first, 100, 1);
    write_frame(second, 101, 2);
    auto reader = details::QueueMessageReader{buffer};

    CHECK_FALSE(reader.has_pending_target());
    CHECK_FALSE(reader.next_target());
    second.commit();
    CHECK_FALSE(reader.has_pending_target());
    CHECK_FALSE(reader.next_target());
    first.commit();

    CHECK(reader.has_pending_target());
    REQUIRE(reader.next_target() == 100);
    REQUIRE(reader.discard(TS<i64>::type()));
    REQUIRE(reader.next_target() == 101);
    REQUIRE(reader.discard(TS<i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("queue message readiness preserves malformed frames for checked reading",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][failure]") {
    const auto buffer = util::buffer::concurrent_chain(32, 1);
    auto writer = util::concurrent_buffer_writer{buffer};
    auto reservation = writer.reserve(sizeof(details::QueueMessageHeader), alignof(details::QueueMessageHeader));
    auto context = ScalarBufferWriteContext{reservation};
    context.write(details::QueueMessageHeader{7, static_cast<std::uint32_t>(sizeof(details::QueueMessageHeader))});
    reservation.commit();

    auto reader = details::QueueMessageReader{buffer};
    CHECK(reader.has_pending_target());
    CHECK_THROWS_AS(reader.next_target(), std::logic_error);
    CHECK(reader.has_pending_target());
    CHECK_THROWS_AS(reader.next_target(), std::logic_error);
}

TEST_CASE("a queue message reader drains while the writer grows its committed watermark",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    constexpr auto message_count = std::size_t{1024};
    const auto block_capacity = GENERATE(std::size_t{65536}, std::size_t{32});
    const auto buffer = util::buffer::concurrent_chain(block_capacity, 4);
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    auto reader = details::QueueMessageReader{buffer};
    auto writer_finished = std::atomic<bool>{};
    auto writes_succeeded = std::atomic<bool>{true};
    auto writer = std::jthread{[&] {
        try {
            auto message_writer = details::QueueMessageWriter{buffer, 37, runtime_type};
            for (auto index = std::size_t{}; index < message_count; ++index) {
                if (!message_writer.write(literal::TimeSeriesLiteral(static_cast<i64>(index)))) {
                    writes_succeeded.store(false, std::memory_order_relaxed);
                    break;
                }
            }
        } catch (...) {
            writes_succeeded.store(false, std::memory_order_relaxed);
        }
        writer_finished.store(true, std::memory_order_release);
    }};

    auto consumed = std::size_t{};
    auto targets_match = true;
    while (consumed < message_count) {
        if (const auto target = reader.next_target()) {
            targets_match = targets_match && *target == 37;
            targets_match = reader.discard(TS<i64>::type()) && targets_match;
            ++consumed;
            continue;
        }

        if (writer_finished.load(std::memory_order_acquire) && !reader.next_target()) {
            break;
        }
        std::this_thread::yield();
    }
    writer.join();

    CHECK(writes_succeeded.load(std::memory_order_relaxed));
    CHECK(consumed == message_count);
    CHECK(targets_match);
    CHECK_FALSE(reader.next_target());
}
