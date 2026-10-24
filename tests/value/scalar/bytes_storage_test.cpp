#include <bg/queue.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/allocation_pool.hpp>
#include <bg/scalar/bytes_builder.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/run_graph.hpp>
#include <bg/graph_run_input.hpp>
#include <bg/time_series/output_state.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/util/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

using namespace bg;

TEST_CASE("bytes scalar values copy borrowed spans and retain shared storage",
          "[feature:scalar-api][feature:scalar-storage][scalar:bytes][storage:variable]") {
    auto source = std::array{byte{0x00}, byte{0xff}, byte{0x7f}};
    const auto borrowed = std::span<const byte>{source};
    const auto value = ScalarValue<bytes>{borrowed};
    const auto *data = value.view().value().data();

    source[1] = byte{0x01};
    CHECK(std::ranges::equal(value.view().value(), std::array{byte{0x00}, byte{0xff}, byte{0x7f}}));

    const auto retained = ScalarValue<bytes>{value.view()};
    CHECK(retained.view().value().data() == data);
    CHECK(retained.equals(value));
    CHECK(retained.hash() == value.hash());

    const auto different = ScalarValue<bytes>{std::array{byte{0x00}, byte{0x01}, byte{0x7f}}};
    CHECK_FALSE(value.equals(different));

    const auto empty = ScalarValue<bytes>{std::span<const byte>{}};
    CHECK(empty.view().value().empty());
}

TEST_CASE("bytes builder exposes writable capacity and transfers finished storage",
          "[feature:scalar-api][feature:scalar-storage][scalar:bytes][storage:variable]") {
    auto allocator = bg::allocator{*std::pmr::new_delete_resource()};
    auto retained = ScalarValue<bytes>{std::span<const byte>{}};
    {
        auto builder = BytesBuilder{allocator, 2};
        auto tail = builder.writable_tail();
        REQUIRE(tail.size() == 2);
        tail[0] = byte{0x10};
        tail[1] = byte{0x20};
        builder.commit(2);

        builder.reserve(5);
        tail = builder.writable_tail();
        REQUIRE(tail.size() == 3);
        tail[0] = byte{0x30};
        tail[1] = byte{0x40};
        tail[2] = byte{0x50};
        builder.commit(3);

        builder.append(builder.view());
        const auto *data = builder.view().data();
        auto value = std::move(builder).finish();
        CHECK(value.view().value().data() == data);
        retained = ScalarValue<bytes>{value.view()};
        CHECK(std::ranges::equal(value.view().value(),
                                 std::array{byte{0x10}, byte{0x20}, byte{0x30}, byte{0x40}, byte{0x50},
                                            byte{0x10}, byte{0x20}, byte{0x30}, byte{0x40}, byte{0x50}}));
    }

    CHECK(std::ranges::equal(retained.view().value(),
                             std::array{byte{0x10}, byte{0x20}, byte{0x30}, byte{0x40}, byte{0x50},
                                        byte{0x10}, byte{0x20}, byte{0x30}, byte{0x40}, byte{0x50}}));
}

TEST_CASE("bytes builder accepts empty values and rejects over-commit",
          "[feature:scalar-api][feature:scalar-storage][scalar:bytes][storage:variable]") {
    auto allocator = bg::allocator{*std::pmr::new_delete_resource()};
    auto empty = std::move(BytesBuilder{allocator, 0}).finish();
    CHECK(empty.view().value().empty());

    auto builder = BytesBuilder{allocator, 1};
    auto tail = builder.writable_tail();
    tail[0] = byte{0x42};
    builder.commit(1);
    CHECK_THROWS_AS(builder.commit(1), std::length_error);
    CHECK(std::ranges::equal(builder.view(), std::array{byte{0x42}}));
}

TEST_CASE("bytes retain their graph allocation owner after the graph releases it",
          "[feature:scalar-storage][scalar:bytes][storage:variable][transition:lifecycle]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    const auto graph = 1;
    auto *allocation_owner = details::ScalarAllocationPool::create(allocator);
    auto retained = std::optional<ScalarValue<bytes>>{};
    {
        const auto context = details::ScalarAllocationContext{allocator, *allocation_owner, &graph, 0};
        auto builder = BytesBuilder{};
        auto tail = builder.writable_tail();
        tail[0] = byte{0x12};
        tail[1] = byte{0x34};
        builder.commit(2);
        retained.emplace(std::move(builder).finish());
    }

    allocation_owner->release();
    REQUIRE(retained.has_value());
    CHECK(std::ranges::equal(retained->view().value(), std::array{byte{0x12}, byte{0x34}}));
    retained.reset();
}

TEST_CASE("lists can own byte scalar values",
          "[feature:scalar-api][feature:scalar-collection][scalar:bytes][scalar:list]") {
    const auto first = std::array{byte{0x00}, byte{0x01}};
    const auto second = std::array{byte{0xfe}};
    const auto source = std::vector{std::span<const byte>{first}, std::span<const byte>{second}};
    const auto value = ScalarValue<List<bytes>>{source};

    REQUIRE(value.view().size() == 2);
    CHECK(std::ranges::equal(value.view()[0].value(), first));
    CHECK(std::ranges::equal(value.view()[1].value(), second));
}

TEST_CASE("queue ticks transfer byte payload ownership into graph storage",
          "[evaluation][feature:time-series-storage][series:value][scalar:bytes][queue]") {
    const auto &runtime_type = cpp_time_series_runtime_type<TS<bytes>>();
    const auto buffer = util::buffer::concurrent_chain(128, 4, allocator{*std::pmr::new_delete_resource()});
    auto control = details::QueueControl{};
    auto queue = control.make_queue(buffer, 5, runtime_type);
    auto builder = BytesBuilder{};
    auto tail = builder.writable_tail();
    tail[0] = byte{0x01};
    tail[1] = byte{0x00};
    tail[2] = byte{0xff};
    builder.commit(3);
    auto payload = std::move(builder).finish();
    const auto *payload_data = payload.view().value().data();
    REQUIRE(queue.write(literal::TimeSeriesLiteral(payload.view())));
    payload = ScalarValue<bytes>{std::span<const byte>{}};

    auto reader = details::QueueMessageReader{buffer};
    REQUIRE(reader.next_target() == 5);
    auto last_updated = TimeSeriesState{};
    details::scalar_stored_t<bytes> stored{};
    const auto now = MIN_DT + std::chrono::microseconds{1};
    auto output = OutputState{runtime_type};
    output.bind(last_updated, &stored, now);
    REQUIRE(reader.apply(output, QueueMode::standard));

    const auto value = Scalar<bytes>::from(cpp_scalar_runtime_type<bytes>(), &stored);
    CHECK(value.value().data() == payload_data);
    CHECK(std::ranges::equal(value.value(), std::array{byte{0x01}, byte{0x00}, byte{0xff}}));
    auto memory = allocator{};
    runtime_type.destroy_value(&stored, memory);
    CHECK_FALSE(reader.next_target());
}

TEST_CASE("a typed graph reads and returns byte payload literals",
          "[testing][evaluation][feature:test-graph][feature:scalar-api][series:value][scalar:bytes]") {
    const auto identity = [](wp<TS<bytes>> input) { return input; };
    const auto first = std::array{byte{0x00}, byte{0x01}};
    const auto second = std::array{byte{0xfe}, byte{0xff}};

    const auto result = bg::run_graph(
        identity, bg::input<TS<bytes>>({std::span<const byte>{first}, std::span<const byte>{second}}));

    REQUIRE(result.size() == 2);
    const auto first_value = result[0];
    const auto second_value = result[1];
    REQUIRE(first_value.has_value());
    REQUIRE(second_value.has_value());
    CHECK(std::ranges::equal(scalar_cast<bytes>(first_value->view()).value(), first));
    CHECK(std::ranges::equal(scalar_cast<bytes>(second_value->view()).value(), second));
}

TEST_CASE("a plain span node parameter resolves to the bytes scalar",
          "[testing][evaluation][feature:test-graph][feature:scalar-api][series:value][scalar:bytes]") {
    STATIC_CHECK(std::same_as<canonical_scalar_t<std::span<const byte>>, bytes>);
    STATIC_CHECK(std::same_as<canonical_scalar_t<std::span<byte, 2>>, bytes>);

    const auto echo = node([](std::span<const byte> value) { return value; });
    const auto payload = std::array{byte{0x00}, byte{0xff}};
    const auto result = bg::run_graph(echo, bg::input<TS<bytes>>({std::span<const byte>{payload}}));

    REQUIRE(result.size() == 1);
    const auto value = result[0];
    REQUIRE(value.has_value());
    CHECK(std::ranges::equal(scalar_cast<bytes>(value->view()).value(), payload));
}
