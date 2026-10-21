#include <bg/dispatch/details/dispatch_selector.hpp>
#include <bg/scalar/bundle/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <vector>

namespace {

using namespace bg;

template <template <typename, auto...> typename Field> struct DispatchBase {
    Field<symbol> name;
};

template <template <typename, auto...> typename Field>
struct DispatchDerived : schema_extends<DispatchBase, Field> {
    Field<i64> value;
};

struct SelectorTestInput {
    struct View {
        const ScalarType *type;

        const ScalarType &concrete_type() const { return *type; }
    };

    bool is_valid;
    const ScalarType *type;

    bool valid() const { return is_valid; }
    View value_view() const { return {type}; }
};

struct SelectorTestOutput {
    bool is_valid{};
    i64 value{};

    void invalidate() { is_valid = false; }
    void set(i64 selected) {
        is_valid = true;
        value = selected;
    }
};

struct SelectorTestInvocation {
    const details::DispatchSelectorData *compiled;
    std::vector<SelectorTestInput> inputs;
    SelectorTestOutput *result;

    std::size_t parameter_count() const { return inputs.size(); }

    template <typename Input> SelectorTestInput input(std::size_t index) const { return inputs[index]; }
    template <typename Data> const Data &compiled_data() const { return *compiled; }
    template <typename Output> SelectorTestOutput &output() { return *result; }
};

TEST_CASE("dispatch selects the most specific applicable scalar type",
          "[feature:dispatch][feature:type-system][binding:covariant]") {
    const auto &base = cpp_scalar_type<Bundle<DispatchBase>>();
    const auto &derived = cpp_scalar_type<Bundle<DispatchDerived>>();

    const auto selected_base = details::select_dispatch_candidate(
        2, 1,
        [&](std::size_t candidate, std::size_t) -> const ScalarType & { return candidate == 0 ? base : derived; },
        [&](std::size_t) -> const ScalarType & { return base; });
    const auto selected_derived = details::select_dispatch_candidate(
        2, 1,
        [&](std::size_t candidate, std::size_t) -> const ScalarType & { return candidate == 0 ? base : derived; },
        [&](std::size_t) -> const ScalarType & { return derived; });

    CHECK(selected_base == 0);
    CHECK(selected_derived == 1);
}

TEST_CASE("dispatch rejects incomparable applicable scalar type tuples",
          "[feature:dispatch][feature:type-system]") {
    const auto &base = cpp_scalar_type<Bundle<DispatchBase>>();
    const auto &derived = cpp_scalar_type<Bundle<DispatchDerived>>();
    const auto candidates = std::array<const ScalarType *, 4>{std::addressof(base), std::addressof(derived),
                                                               std::addressof(derived), std::addressof(base)};

    CHECK_THROWS_AS(details::select_dispatch_candidate(
                        2, 2,
                        [&](std::size_t candidate, std::size_t input) -> const ScalarType & {
                            return *candidates[candidate * 2 + input];
                        },
                        [&](std::size_t) -> const ScalarType & { return derived; }),
                    std::logic_error);
}

TEST_CASE("dispatch rejects an unmatched concrete scalar type", "[feature:dispatch][feature:type-system]") {
    const auto &base = cpp_scalar_type<Bundle<DispatchBase>>();
    const auto &unrelated = int64_type();
    CHECK_THROWS_AS(details::select_dispatch_candidate(
                        1, 1,
                        [&](std::size_t, std::size_t) -> const ScalarType & { return base; },
                        [&](std::size_t) -> const ScalarType & { return unrelated; }),
                    std::out_of_range);
}

TEST_CASE("dispatch compares every selected input when finding a more specific tuple",
          "[feature:dispatch][feature:type-system]") {
    const auto &base = cpp_scalar_type<Bundle<DispatchBase>>();
    const auto &derived = cpp_scalar_type<Bundle<DispatchDerived>>();
    const auto candidates = std::array<const ScalarType *, 8>{
        std::addressof(base), std::addressof(base), std::addressof(derived), std::addressof(base),
        std::addressof(base), std::addressof(derived), std::addressof(derived), std::addressof(derived)};

    const auto selected = details::select_dispatch_candidate(
        4, 2,
        [&](std::size_t candidate, std::size_t input) -> const ScalarType & {
            return *candidates[candidate * 2 + input];
        },
        [&](std::size_t) -> const ScalarType & { return derived; });

    CHECK(selected == 3);
}

TEST_CASE("dispatch leaves output invalid when a selected input is invalid",
          "[feature:dispatch][feature:type-system][validity:optional]") {
    const auto &base = cpp_scalar_type<Bundle<DispatchBase>>();
    auto compiled = details::DispatchSelectorData{};
    compiled.input_count = 1;
    compiled.input_types.push_back(&base);
    auto result = SelectorTestOutput{.is_valid = true, .value = 9};
    auto invocation = SelectorTestInvocation{
        .compiled = &compiled,
        .inputs = {{.is_valid = false, .type = &base}},
        .result = &result,
    };

    details::DispatchSelectorDefinition::evaluate(invocation);

    CHECK_FALSE(result.is_valid);
}

} // namespace
