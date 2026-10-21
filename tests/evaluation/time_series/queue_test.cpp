#include <bg/queue.hpp>
#include <bg/details/engine_wakeup.hpp>
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
#include <barrier>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <thread>

using namespace bg;
namespace {

bool wait_for_notification(details::EngineWakeup &wakeup) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (wakeup.wait_until(deadline)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("queue writes publish committed messages before notifying the engine",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][wakeup]") {
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    const auto buffer = util::buffer::concurrent_chain(64, 4, allocator{*std::pmr::new_delete_resource()});
    auto wakeup = details::EngineWakeup{};
    auto control = details::QueueControl{&wakeup};
    auto queue = control.make_queue(buffer, 19, runtime_type);
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(i64{71})));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(i64{72})));
    REQUIRE(wait_for_notification(wakeup));
    CHECK_FALSE(wakeup.wait_until(std::chrono::steady_clock::now()));

    auto reader = details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 19);
    REQUIRE(reader.discard(TS<i64>::type()));
    REQUIRE(reader.next_target() == 19);
    REQUIRE(reader.discard(TS<i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("engine wakeups preserve early notifications and coalesce concurrent notifications",
          "[coverage-exempt][queue][wakeup]") {
    auto wakeup = details::EngineWakeup{};
    CHECK_FALSE(wakeup.wait_until(std::chrono::steady_clock::now()));

    wakeup.notify();
    wakeup.wait();
    wakeup.notify();
    REQUIRE(wait_for_notification(wakeup));
    CHECK_FALSE(wakeup.wait_until(std::chrono::steady_clock::now()));

    constexpr auto writer_count = std::size_t{4};
    auto writers = std::array<std::jthread, writer_count>{};
    for (auto &writer : writers) {
        writer = std::jthread{[&] {
            for (auto count = std::size_t{}; count < 128; ++count) {
                wakeup.notify();
            }
        }};
    }
    for (auto &writer : writers) {
        writer.join();
    }

    REQUIRE(wait_for_notification(wakeup));
    CHECK_FALSE(wakeup.wait_until(std::chrono::steady_clock::now()));
    CHECK_FALSE(wakeup.wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds{10}));
}

TEST_CASE("accepted queue writes that throw still wake the engine",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][wakeup][failure]") {
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    const auto buffer = util::buffer::concurrent_chain(64, 4, allocator{*std::pmr::new_delete_resource()});
    auto wakeup = details::EngineWakeup{};
    auto control = details::QueueControl{&wakeup};
    auto queue = control.make_queue(buffer, 20, runtime_type);
    CHECK_THROWS_AS(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)), std::invalid_argument);
    REQUIRE(wait_for_notification(wakeup));
    auto reader = details::QueueMessageReader{buffer};
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("the engine consumes committed queue messages while writers keep notifying",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][wakeup][concurrency]") {
    constexpr auto writer_count = std::size_t{4};
    constexpr auto writes_per_writer = std::size_t{512};
    const auto expected_count = writer_count * writes_per_writer;
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    const auto buffer = util::buffer::concurrent_chain(64, 4, allocator{*std::pmr::new_delete_resource()});
    auto wakeup = details::EngineWakeup{};
    auto control = details::QueueControl{&wakeup};
    auto root = control.make_queue(buffer, 21, runtime_type);
    auto queues = std::array{root, root, root, root};
    auto writer_errors = std::array<std::exception_ptr, writer_count>{};
    auto writers = std::array<std::jthread, writer_count>{};
    auto start = std::barrier{static_cast<std::ptrdiff_t>(writer_count + 1)};

    for (auto index = std::size_t{}; index < writer_count; ++index) {
        writers[index] = std::jthread{[&, index] {
            start.arrive_and_wait();
            try {
                for (auto value = std::size_t{}; value < writes_per_writer; ++value) {
                    const auto payload = static_cast<i64>(index * writes_per_writer + value);
                    if (!queues[index].write(::literal::TimeSeriesLiteral(payload))) {
                        throw std::logic_error{"open queue rejected a concurrent write"};
                    }
                }
            } catch (...) {
                writer_errors[index] = std::current_exception();
            }
        }};
    }

    start.arrive_and_wait();
    auto reader = details::QueueMessageReader{buffer};
    auto messages_read = std::size_t{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (messages_read < expected_count && std::chrono::steady_clock::now() < deadline) {
        if (!reader.next_target()) {
            (void)wakeup.wait_until(deadline);
            continue;
        }

        REQUIRE(reader.discard(TS<i64>::type()));
        ++messages_read;
    }

    for (auto &writer : writers) {
        writer.join();
    }
    CHECK(messages_read == expected_count);
    for (const auto &error : writer_errors) {
        CHECK(error == nullptr);
    }
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("queue copies write independently to their destination",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    const auto buffer = util::buffer::concurrent_chain(32, 4, allocator{*std::pmr::new_delete_resource()});
    auto control = details::QueueControl{};
    auto empty = Queue{};
    CHECK_FALSE(empty.is_open());
    CHECK_FALSE(empty.write(::literal::TimeSeriesLiteral(::literal::SKIP)));
    auto first = control.make_queue(buffer, 17, runtime_type);
    CHECK(first.is_open());
    auto second = first;
    auto moved = std::move(second);
    CHECK_FALSE(second.is_open());
    CHECK(moved.is_open());
    CHECK_FALSE(second.write(::literal::TimeSeriesLiteral(i64{13})));
    second = first;

    CHECK_FALSE(first.write(::literal::TimeSeriesLiteral(::literal::SKIP)));
    REQUIRE(first.write(::literal::TimeSeriesLiteral(i64{11})));
    REQUIRE(moved.write(::literal::TimeSeriesLiteral(i64{12})));
    REQUIRE(second.write(::literal::TimeSeriesLiteral(i64{13})));
    control.close();
    CHECK_FALSE(first.is_open());
    CHECK_FALSE(moved.is_open());

    auto reader = details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 17);
    REQUIRE(reader.discard(TS<i64>::type()));
    REQUIRE(reader.next_target() == 17);
    REQUIRE(reader.discard(TS<i64>::type()));
    REQUIRE(reader.next_target() == 17);
    REQUIRE(reader.discard(TS<i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("queue closure rejects malformed writes before serialization and releases failed writes",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    const auto buffer = util::buffer::concurrent_chain(64, 4, allocator{*std::pmr::new_delete_resource()});
    auto control = details::QueueControl{};
    auto queue = control.make_queue(buffer, 8, runtime_type);

    CHECK_THROWS_AS(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)), std::invalid_argument);
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(i64{23})));
    control.close();
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(i64{24})));

    auto reader = details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 8);
    REQUIRE(reader.discard(TS<i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("destroying queue control closes retained handles",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    const auto buffer = util::buffer::concurrent_chain(64, 4, allocator{*std::pmr::new_delete_resource()});
    auto queue = Queue{};
    {
        auto control = details::QueueControl{};
        queue = control.make_queue(buffer, 29, runtime_type);
        REQUIRE(queue.write(::literal::TimeSeriesLiteral(i64{31})));
    }

    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(i64{32})));
    auto copied_closed = queue;
    CHECK_FALSE(copied_closed.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));

    auto reader = details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 29);
    REQUIRE(reader.discard(TS<i64>::type()));
    CHECK_FALSE(reader.next_target());
}

namespace {

class LocalRuntimeType final : public TimeSeriesValueRuntimeType {
  public:
    LocalRuntimeType()
        : TimeSeriesValueRuntimeType{
              TS<i64>::type(), cpp_scalar_runtime_type<i64>(), sizeof(i64), alignof(i64), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<i64 *>(destination), i64{});
    }
    void destroy_value(void *value, allocator &) const noexcept override { std::destroy_at(static_cast<i64 *>(value)); }
};

} // namespace

TEST_CASE("closed queue handles do not access an expired runtime type",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    const auto buffer = util::buffer::concurrent_chain(64, 4, allocator{*std::pmr::new_delete_resource()});
    auto control = details::QueueControl{};
    auto queue = Queue{};
    {
        auto runtime_type = LocalRuntimeType{};
        queue = control.make_queue(buffer, 33, runtime_type);
        control.close();
    }

    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(i64{1})));
    auto copy = queue;
    CHECK_FALSE(copy.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    CHECK_FALSE(copy.write(::literal::TimeSeriesLiteral(i64{2})));
}

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

} // namespace

TEST_CASE("queue control close waits for accepted writes",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    auto memory = BlockingResource{};
    const auto buffer = util::buffer::concurrent_chain(1, 4, allocator{memory});
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    auto queue = Queue{};
    {
        auto wakeup = details::EngineWakeup{};
        auto control = details::QueueControl{&wakeup};
        queue = control.make_queue(buffer, 41, runtime_type);
        auto probe = queue;
        memory.block_next_allocation();

        auto write_finished = std::atomic<bool>{};
        auto write_error = std::exception_ptr{};
        auto close_finished = std::atomic<bool>{};
        auto closer = std::jthread{};
        auto writer = std::jthread{[&] {
            try {
                if (!queue.write(::literal::TimeSeriesLiteral(i64{7}))) {
                    throw std::logic_error{"accepted queue write was rejected"};
                }
            } catch (...) {
                write_error = std::current_exception();
            }
            write_finished.store(true, std::memory_order_release);
            write_finished.notify_all();
        }};
        auto release = util::scope_exit{[&]() noexcept { memory.release_allocation(); }};
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
    }

    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(i64{8})));

    auto reader = details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 41);
    REQUIRE(reader.discard(TS<i64>::type()));
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("queue copies reserve across concurrent block rollover",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue]") {
    constexpr auto writer_count = std::size_t{4};
    constexpr auto writes_per_writer = std::size_t{128};
    const auto &runtime_type = cpp_time_series_runtime_type<TS<i64>>();
    const auto buffer = util::buffer::concurrent_chain(32, 4, allocator{*std::pmr::new_delete_resource()});
    auto control = details::QueueControl{};
    auto root = control.make_queue(buffer, 0, runtime_type);
    auto queues = std::array{root, root, root, root};
    auto writer_errors = std::array<std::exception_ptr, writer_count>{};
    auto writers = std::array<std::jthread, writer_count>{};

    for (auto index = std::size_t{}; index < writer_count; ++index) {
        writers[index] = std::jthread{[&, index] {
            try {
                for (auto value = std::size_t{}; value < writes_per_writer; ++value) {
                    const auto payload = static_cast<i64>(index * writes_per_writer + value);
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

    auto reader = details::QueueMessageReader{buffer};
    auto evaluation_time = MIN_DT + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{evaluation_time};
    auto payload = i64{};
    auto output = OutputState{runtime_type};
    output.bind(last_updated, &payload, evaluation_time);
    auto next_value = std::array<std::size_t, writer_count>{};
    for (auto count = std::size_t{}; count < writer_count * writes_per_writer; ++count) {
        REQUIRE(reader.next_target() == 0);
        evaluation_time += std::chrono::microseconds{1};
        REQUIRE(reader.apply(output, QueueMode::standard));
        const auto writer_index = static_cast<std::size_t>(payload) / writes_per_writer;
        REQUIRE(writer_index < writer_count);
        CHECK(payload == static_cast<i64>(writer_index * writes_per_writer + next_value[writer_index]));
        ++next_value[writer_index];
    }
    for (const auto next : next_value) {
        CHECK(next == writes_per_writer);
    }
    CHECK_FALSE(reader.next_target());
}
