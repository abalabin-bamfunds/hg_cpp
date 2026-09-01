#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
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

} // namespace
