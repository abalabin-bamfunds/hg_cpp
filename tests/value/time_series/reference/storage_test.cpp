#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace bg;

TEST_CASE("a time series reference value is one pointer",
          "[value][time-series][reference][feature:time-series-storage]") {
    STATIC_CHECK(sizeof(Reference) == sizeof(void *));
    STATIC_CHECK(sizeof(details::RootReference) == 5 * sizeof(void *));

    const auto &reference_type = REF<TS<i64>>::type();
    const auto &runtime_type = time_series_reference_runtime_type(reference_type);

    CHECK(runtime_type.value_size() == sizeof(Reference));
    CHECK(runtime_type.value_alignment() == alignof(Reference));
}

TEST_CASE("runtime types identify reference storage without a runtime downcast",
          "[value][time-series][reference][feature:time-series-storage]") {
    CHECK(cpp_time_series_runtime_type<REF<TS<i64>>>().is_reference());
    CHECK_FALSE(cpp_time_series_runtime_type<TS<i64>>().is_reference());
}

TEST_CASE("a time series reference propagates its value",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto root = details::RootReference{};
    auto input_reference = Reference{&root};
    auto output_value = Reference{};
    const auto input_time = EvaluationTime{EvaluationTime::duration{1}};
    const auto input_state = TimeSeriesState{input_time};
    auto output_state = TimeSeriesState{};
    const auto evaluation_time = EvaluationTime{EvaluationTime::duration{2}};
    const auto &runtime_type = cpp_time_series_runtime_type<REF<TS<i64>>>();

    const auto input = In<REF<TS<i64>>>{input_state, &input_reference};
    auto state = details::OutputState{runtime_type};
    state.bind(output_state, &output_value, evaluation_time);
    const auto output = details::output_factory::make<Out<REF<TS<i64>>>>(state);
    output.copy(input);

    CHECK(output_value == input_reference);
    CHECK(output_state.time() == evaluation_time);
}

TEST_CASE("an item reference retains its parent and item ID",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto root = details::RootReference{};
    auto outer = details::ItemReference{Reference{&root}, 12};
    auto inner = details::ItemReference{Reference{&outer}, 34};

    CHECK(inner.parent().get<details::ItemReference>() == &outer);
    CHECK(inner.item_id() == 34);
    CHECK(outer.parent().get<details::RootReference>() == &root);
    CHECK(outer.item_id() == 12);
}

TEST_CASE("a representation reference retains its base reference and representation index",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto root = details::RootReference{};
    auto item = details::ItemReference{Reference{&root}, 12};
    auto representation = details::RepresentationReference{Reference{&item}, 7};

    CHECK(representation.base().get<details::ItemReference>() == &item);
    CHECK(representation.representation_index() == 7);
    STATIC_CHECK(sizeof(details::ItemReference) == 3 * sizeof(void *));
    STATIC_CHECK(sizeof(details::RepresentationReference) == 3 * sizeof(void *));
}

TEST_CASE("reference subscriber group encoding is hidden behind accessors",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto subscriptions = details::ReferenceSubscriptionDetail{};

    CHECK_FALSE(subscriptions.has_subscriber_group());

    subscriptions.set_subscriber_group(7);

    CHECK(subscriptions.has_subscriber_group());
    CHECK(subscriptions.subscriber_group_index() == 7);

    subscriptions.clear_subscriber_group();

    CHECK_FALSE(subscriptions.has_subscriber_group());
}

TEST_CASE("reference item notification descriptor encoding is hidden behind accessors",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto notifications = details::ItemNotificationDescriptor{};

    CHECK(notifications.empty());

    notifications.set_inline_destination_item(4, 12);

    CHECK_FALSE(notifications.empty());
    CHECK_FALSE(notifications.has_destination_item_group());
    CHECK(notifications.inline_destination_index() == 4);
    CHECK(notifications.inline_destination_item_id() == 12);

    notifications.set_destination_item_group(9);

    CHECK(notifications.has_destination_item_group());
    CHECK(notifications.destination_item_group_index() == 9);

    notifications.clear();

    CHECK(notifications.empty());
    STATIC_CHECK(sizeof(details::ItemNotificationDescriptor) == sizeof(void *));
}
