#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_input_view.hpp>
#include <bg/time_series/tsd/tsd_output_view.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

namespace {

struct RuntimeNodeState {
    std::int64_t evaluations{};
};

struct RuntimeNodeData {
    std::int64_t factor;
};

struct RuntimeNodeDefinition {
    using compiled_data_type = RuntimeNodeData;
    using state_type = RuntimeNodeState;

    RuntimeNodeDefinition()
        : signature_{
              bg::util::symbol{"runtime_node"},
              {{bg::util::symbol{"input"},
                &bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(), bg::TimeSeriesGuarantee::ordinary)}},
              bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(), bg::TimeSeriesGuarantee::ordinary)} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    [[nodiscard]] RuntimeNodeData compile(const bg::ResolvedSignature &, std::pmr::memory_resource &) const {
        return {.factor = 2};
    }

    [[nodiscard]] const bg::TimeSeriesStorageRepresentation *
    output_storage_representation(const bg::ResolvedSignature &,
                                  std::span<const bg::ScalarStorageRepresentation *const>,
                                  std::span<const bg::TimeSeriesStorageRepresentation *const> inputs) const noexcept {
        return inputs.front();
    }

    static void evaluate(bg::NodeInvocation<compiled_data_type, state_type> invocation) {
        const auto input = *invocation.inputs().begin();
        CHECK(input.parameter_index() == 0);
        CHECK(input.parameter_name() == bg::util::symbol{"input"});
        CHECK(input.parameter_type().is_time_series());

        const auto value = input.value();
        const auto &view = static_cast<const bg::TimeSeriesValueInputView &>(*value.operator->());
        const auto number = bg::scalar_cast<std::int64_t>(view.value_view()).value();
        const auto result = number * invocation.compiled_data().factor + ++invocation.state().evaluations;
        static_cast<const bg::TypedTimeSeriesValueOutputView<std::int64_t> &>(*invocation.output().operator->())
            .set(result);
    }

    bg::Signature signature_;
};

TEST_CASE("a runtime-signature node exposes resolved parameters, compiled data, and state",
          "[node][runtime-signature]") {
    const auto runtime_node = bg::node(RuntimeNodeDefinition{});

    const auto graph = [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        return bg::wp<bg::TS<std::int64_t>>{*runtime_node(input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2, 3}).equals({3, 6, 9}));
}

TEST_CASE("graph wiring owns a temporary runtime-signature node template", "[node][runtime-signature][lifetime]") {
    const auto graph = [](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        return bg::wp<bg::TS<std::int64_t>>{*bg::node(RuntimeNodeDefinition{})(input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({3, 6}));
}

struct RuntimeSinkDefinition {
    RuntimeSinkDefinition()
        : signature_{bg::util::symbol{"runtime_sink"},
                     {{bg::util::symbol{"input"}, &bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(),
                                                                              bg::TimeSeriesGuarantee::ordinary)}}} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    [[nodiscard]] const bg::TimeSeriesStorageRepresentation *
    output_storage_representation(const bg::ResolvedSignature &,
                                  std::span<const bg::ScalarStorageRepresentation *const>,
                                  std::span<const bg::TimeSeriesStorageRepresentation *const>) const noexcept {
        return nullptr;
    }

    static void evaluate(bg::NodeInvocation<> invocation) {
        CHECK(invocation.inputs().begin() != invocation.inputs().end());
    }

    bg::Signature signature_;
};

TEST_CASE("a runtime-signature node can omit its output", "[node][runtime-signature][output]") {
    const auto sink = bg::node(RuntimeSinkDefinition{});
    const auto graph = [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        CHECK_FALSE(sink(input).has_value());
        return input;
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({1, 2}));
}

struct OptionalScalarDefinition {
    OptionalScalarDefinition()
        : signature_{
              bg::util::symbol{"optional_scalar"},
              {{bg::util::symbol{"input"},
                &bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(), bg::TimeSeriesGuarantee::ordinary)},
               {bg::util::symbol{"fallback"}, &bg::cpp_scalar_type<std::int64_t>(), bg::active, bg::required, true}},
              bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(), bg::TimeSeriesGuarantee::ordinary)} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    [[nodiscard]] const bg::TimeSeriesStorageRepresentation *
    output_storage_representation(const bg::ResolvedSignature &,
                                  std::span<const bg::ScalarStorageRepresentation *const>,
                                  std::span<const bg::TimeSeriesStorageRepresentation *const> inputs) const noexcept {
        return inputs.front();
    }

    static void evaluate(bg::NodeInvocation<> invocation) {
        const auto scalar = *invocation.scalars().begin();
        CHECK(scalar.parameter_index() == 1);
        CHECK(scalar.parameter_name() == bg::util::symbol{"fallback"});

        const auto input = (*invocation.inputs().begin()).value();
        const auto &view = static_cast<const bg::TimeSeriesValueInputView &>(*input.operator->());
        const auto number = bg::scalar_cast<std::int64_t>(view.value_view()).value();
        const auto result = scalar.has_value() ? -1 : number;
        static_cast<const bg::TypedTimeSeriesValueOutputView<std::int64_t> &>(*invocation.output().operator->())
            .set(result);
    }

    bg::Signature signature_;
};

TEST_CASE("an absent optional scalar remains enumerable", "[node][runtime-signature][scalar]") {
    const auto runtime_node = bg::node(OptionalScalarDefinition{});

    const auto graph = [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        return bg::wp<bg::TS<std::int64_t>>{*runtime_node(input, std::optional<std::int64_t>{})};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({1, 2}));
}

struct RuntimeDictionarySourceDefinition {
    RuntimeDictionarySourceDefinition()
        : output_type_{&bg::time_series_dictionary_type(
              bg::cpp_scalar_type<std::int64_t>(),
              bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(), bg::TimeSeriesGuarantee::ordinary),
              bg::TimeSeriesGuarantee::ordinary)},
          signature_{bg::util::symbol{"runtime_dictionary_source"}, {}, *output_type_} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    [[nodiscard]] const bg::TimeSeriesStorageRepresentation *
    output_storage_representation(const bg::ResolvedSignature &signature,
                                  std::span<const bg::ScalarStorageRepresentation *const> scalars,
                                  std::span<const bg::TimeSeriesStorageRepresentation *const> inputs) const noexcept {
        return &bg::resolve_time_series_storage_representation(*signature.output, inputs, scalars);
    }

    static void start(bg::NodeInvocation<> invocation) {
        auto output = invocation.output();
        const auto &dictionary = static_cast<const bg::TimeSeriesDictionaryOutputView &>(*output.operator->());
        const auto key = bg::ScalarValue<std::int64_t>{1};
        const auto item = dictionary.item(key.input_view());
        auto item_view = bg::util::polymorphic_value<bg::TimeSeriesOutputView>::copy(item.prototype);
        item_view->bind(item.last_updated, item.value, item.output_path);
        static_cast<const bg::TypedTimeSeriesValueOutputView<std::int64_t> &>(item_view.get()).set(42);
        item_view->release_output_path();
    }

    const bg::TimeSeriesDictionaryType *output_type_;
    bg::Signature signature_;
};

struct RuntimeDictionarySinkDefinition {
    bg::In<bg::TSD<std::int64_t, bg::TS<std::int64_t>>> input;

    void evaluate() const { value = input[1].value(); }

    inline static std::int64_t value{};
};

TEST_CASE("a runtime-signature node constructs runtime-defined dictionary storage",
          "[node][runtime-signature][tsd]") {
    const auto source = bg::node(RuntimeDictionarySourceDefinition{});
    const auto sink = bg::node<RuntimeDictionarySinkDefinition>();
    RuntimeDictionarySinkDefinition::value = 0;

    bg::run_graph([&] { sink(bg::wp<bg::TSD<std::int64_t, bg::TS<std::int64_t>>>{*source()}); });

    CHECK(RuntimeDictionarySinkDefinition::value == 42);
}

} // namespace
