#include <bg/queue.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/buffer.hpp>
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <thread>

namespace {

class BlockingResource : public std::pmr::memory_resource {
  public:
    void block_next_allocation() noexcept { block_.store(true, std::memory_order_release); }
    void wait_until_blocked() noexcept {
        while (!entered_.load(std::memory_order_acquire)) {
            entered_.wait(false, std::memory_order_acquire);
        }
    }
    void release_allocation() noexcept {
        released_.store(true, std::memory_order_release);
        released_.notify_all();
    }

  private:
    void *do_allocate(std::size_t size, std::size_t alignment) override {
        if (block_.exchange(false, std::memory_order_acq_rel)) {
            entered_.store(true, std::memory_order_release);
            entered_.notify_all();
            while (!released_.load(std::memory_order_acquire)) {
                released_.wait(false, std::memory_order_acquire);
            }
        }
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }

    void do_deallocate(void *memory, std::size_t size, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(memory, size, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }

    std::atomic<bool> block_{};
    std::atomic<bool> entered_{};
    std::atomic<bool> released_{};
};

class LocalRuntimeType final : public bg::TimeSeriesValueRuntimeType {
  public:
    LocalRuntimeType()
        : TimeSeriesValueRuntimeType{bg::TS<bg::i64>::type(), bg::cpp_scalar_runtime_type<bg::i64>(),
                                     sizeof(bg::i64), alignof(bg::i64), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<bg::i64 *>(destination), bg::i64{});
    }
    void destroy_value(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<bg::i64 *>(value));
    }
};

} // namespace

TEST_CASE("queue copies write independently to their destination",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    const auto buffer = bg::util::buffer::concurrent_chain(32, 4,
                                                           bg::allocator{*std::pmr::new_delete_resource()});
    auto control = bg::details::QueueControl{};
    auto empty = bg::Queue{};
    CHECK_FALSE(empty.write(::literal::TimeSeriesLiteral(::literal::SKIP)));
    auto first = control.make_queue(buffer, 17, runtime_type);
    auto second = first;
    auto moved = std::move(second);
    CHECK_FALSE(second.write(::literal::TimeSeriesLiteral(bg::i64{13})));
    second = first;

    CHECK_FALSE(first.write(::literal::TimeSeriesLiteral(::literal::SKIP)));
    REQUIRE(first.write(::literal::TimeSeriesLiteral(bg::i64{11})));
    REQUIRE(moved.write(::literal::TimeSeriesLiteral(bg::i64{12})));
    REQUIRE(second.write(::literal::TimeSeriesLiteral(bg::i64{13})));

    auto reader = bg::details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 17);
    REQUIRE(reader.discard(bg::TS<bg::i64>::type()));
    REQUIRE(reader.next_target() == 17);
    REQUIRE(reader.discard(bg::TS<bg::i64>::type()));
    REQUIRE(reader.next_target() == 17);
    REQUIRE(reader.discard(bg::TS<bg::i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("queue closure rejects malformed writes before serialization and releases failed writes",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    const auto buffer = bg::util::buffer::concurrent_chain(64, 4,
                                                           bg::allocator{*std::pmr::new_delete_resource()});
    auto control = bg::details::QueueControl{};
    auto queue = control.make_queue(buffer, 8, runtime_type);

    CHECK_THROWS_AS(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)), std::invalid_argument);
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{23})));
    control.close();
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(bg::i64{24})));

    auto reader = bg::details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 8);
    REQUIRE(reader.discard(bg::TS<bg::i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("destroying queue control closes retained handles",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    const auto buffer = bg::util::buffer::concurrent_chain(64, 4,
                                                           bg::allocator{*std::pmr::new_delete_resource()});
    auto queue = bg::Queue{};
    {
        auto control = bg::details::QueueControl{};
        queue = control.make_queue(buffer, 29, runtime_type);
        REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{31})));
    }

    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(bg::i64{32})));
    auto copied_closed = queue;
    CHECK_FALSE(copied_closed.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));

    auto reader = bg::details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 29);
    REQUIRE(reader.discard(bg::TS<bg::i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("closed queue handles do not access an expired runtime type",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto buffer = bg::util::buffer::concurrent_chain(64, 4,
                                                           bg::allocator{*std::pmr::new_delete_resource()});
    auto control = bg::details::QueueControl{};
    auto queue = bg::Queue{};
    {
        auto runtime_type = LocalRuntimeType{};
        queue = control.make_queue(buffer, 33, runtime_type);
        control.close();
    }

    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(bg::i64{1})));
    auto copy = queue;
    CHECK_FALSE(copy.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    CHECK_FALSE(copy.write(::literal::TimeSeriesLiteral(bg::i64{2})));
}

TEST_CASE("queue control close waits for accepted writes",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    auto memory = BlockingResource{};
    const auto buffer = bg::util::buffer::concurrent_chain(1, 4, bg::allocator{memory});
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    auto control = bg::details::QueueControl{};
    auto queue = control.make_queue(buffer, 41, runtime_type);
    auto probe = queue;
    memory.block_next_allocation();

    auto write_finished = std::atomic<bool>{};
    auto write_error = std::exception_ptr{};
    auto close_finished = std::atomic<bool>{};
    auto closer = std::jthread{};
    auto writer = std::jthread{[&] {
        try {
            if (!queue.write(::literal::TimeSeriesLiteral(bg::i64{7}))) {
                throw std::logic_error{"accepted queue write was rejected"};
            }
        } catch (...) {
            write_error = std::current_exception();
        }
        write_finished.store(true, std::memory_order_release);
        write_finished.notify_all();
    }};
    auto release = bg::util::scope_exit{[&]() noexcept { memory.release_allocation(); }};
    memory.wait_until_blocked();

    closer = std::jthread{[&] {
        control.close();
        close_finished.store(true, std::memory_order_release);
        close_finished.notify_all();
    }};

    while (true) {
        try {
            if (!probe.write(::literal::TimeSeriesLiteral(::literal::REMOVE))) {
                break;
            }
        } catch (const std::invalid_argument &) {
            continue;
        }
    }

    CHECK_FALSE(close_finished.load(std::memory_order_acquire));
    CHECK_FALSE(write_finished.load(std::memory_order_acquire));
    memory.release_allocation();
    release.release();
    writer.join();
    closer.join();
    CHECK(write_error == nullptr);
    CHECK(close_finished.load(std::memory_order_acquire));

    auto reader = bg::details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 41);
    REQUIRE(reader.discard(bg::TS<bg::i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("queue copies reserve across concurrent block rollover",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    constexpr auto writer_count = std::size_t{4};
    constexpr auto writes_per_writer = std::size_t{128};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    const auto buffer = bg::util::buffer::concurrent_chain(32, 4,
                                                           bg::allocator{*std::pmr::new_delete_resource()});
    auto control = bg::details::QueueControl{};
    auto root = control.make_queue(buffer, 0, runtime_type);
    auto queues = std::array{root, root, root, root};
    auto writer_errors = std::array<std::exception_ptr, writer_count>{};
    auto writers = std::array<std::jthread, writer_count>{};

    for (auto index = std::size_t{}; index < writer_count; ++index) {
        writers[index] = std::jthread{[&, index] {
            try {
                for (auto value = std::size_t{}; value < writes_per_writer; ++value) {
                    const auto payload = static_cast<bg::i64>(index * writes_per_writer + value);
                    if (!queues[index].write(::literal::TimeSeriesLiteral(payload))) {
                        throw std::logic_error{"open queue rejected a stress write"};
                    }
                }
            } catch (...) {
                writer_errors[index] = std::current_exception();
            }
        }};
    }
    for (auto &writer : writers) {
        writer.join();
    }
    for (const auto &error : writer_errors) {
        CHECK(error == nullptr);
    }

    auto reader = bg::details::QueueMessageReader{buffer};
    auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto last_updated = bg::TimeSeriesState{evaluation_time};
    auto payload = bg::i64{};
    auto output = bg::OutputState{runtime_type};
    output.bind(last_updated, &payload, evaluation_time);
    auto next_value = std::array<std::size_t, writer_count>{};
    for (auto count = std::size_t{}; count < writer_count * writes_per_writer; ++count) {
        REQUIRE(reader.next_target() == 0);
        evaluation_time += std::chrono::microseconds{1};
        REQUIRE(reader.apply(output, bg::QueueMode::standard));
        const auto writer_index = static_cast<std::size_t>(payload) / writes_per_writer;
        REQUIRE(writer_index < writer_count);
        CHECK(payload == static_cast<bg::i64>(writer_index * writes_per_writer + next_value[writer_index]));
        ++next_value[writer_index];
    }
    for (const auto next : next_value) {
        CHECK(next == writes_per_writer);
    }
    CHECK_FALSE(reader.next_target());
}
