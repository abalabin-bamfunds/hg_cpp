#include <hg/hg.hpp>
#include <hg/scalar/data.hpp>
#include <hg/run_graph.hpp>
#include <hg/time_series/cpp_runtime_type.hpp>
#include <hg/time_series/input_data.hpp>
#include <hg/time_series/output_binding.hpp>
#include <hg/time_series/output_state.hpp>
#include <hg/time_series/time_series_input.hpp>
#include <hg/time_series/time_series_output.hpp>
#include <hg/time_series/time_series_runtime_type.hpp>
#include <hg/time_series/time_series_state.hpp>
#include <hg/time_series/time_series_type.hpp>
#include <hg/time_series/time_series_type_traits.hpp>
#include <hg/node/output_declaration_traits.hpp>
#include <hg/type/type_map.hpp>
#include <hg/type/type_registry.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace provider {

inline bool *view_invalidation_observer{};
struct view_option {};
inline constexpr view_option view{};

template <typename Item, auto... Properties>
struct Series : hg::TimeSeriesDeclaration<Series<Item, Properties...>, Properties...> {};

class SeriesType final : public hg::TimeSeriesType {
  public:
    explicit SeriesType(hg::GuaranteeParameter guarantee) : TimeSeriesType{guarantee} {}

    bool is_resolved() const noexcept override { return guarantee().is_resolved(); }
    const hg::TimeSeriesType &with_guarantee(hg::TimeSeriesGuarantee guarantee) const override;
    const hg::Type &resolve(const hg::TypeMap &type_map) const override;

  protected:
    bool do_match(const hg::Type &supplied, hg::TypeMap &type_map) const override;
};

const SeriesType &series_type(hg::TimeSeriesGuarantee guarantee) {
    static const auto ordinary = SeriesType{hg::TimeSeriesGuarantee::ordinary};
    static const auto deduplicated = SeriesType{hg::TimeSeriesGuarantee::deduplicated};
    static const auto final = SeriesType{hg::TimeSeriesGuarantee::final};
    static const auto constant = SeriesType{hg::TimeSeriesGuarantee::const_};
    [[maybe_unused]] static const auto &ordinary_registered = hg::TypeRegistry::register_static(ordinary);
    [[maybe_unused]] static const auto &deduplicated_registered = hg::TypeRegistry::register_static(deduplicated);
    [[maybe_unused]] static const auto &final_registered = hg::TypeRegistry::register_static(final);
    [[maybe_unused]] static const auto &constant_registered = hg::TypeRegistry::register_static(constant);

    switch (guarantee) {
    case hg::TimeSeriesGuarantee::ordinary:
        return ordinary_registered;
    case hg::TimeSeriesGuarantee::deduplicated:
        return deduplicated_registered;
    case hg::TimeSeriesGuarantee::final:
        return final_registered;
    case hg::TimeSeriesGuarantee::const_:
        return constant_registered;
    }

    std::unreachable();
}

const hg::Type &SeriesType::resolve(const hg::TypeMap &type_map) const {
    return series_type(guarantee().resolve(type_map));
}

const hg::TimeSeriesType &SeriesType::with_guarantee(hg::TimeSeriesGuarantee guarantee) const {
    return series_type(guarantee);
}

bool SeriesType::do_match(const hg::Type &supplied, hg::TypeMap &type_map) const {
    const auto *series = dynamic_cast<const SeriesType *>(&supplied);
    return series != nullptr && series->is_resolved() &&
           guarantee().match(*series->guarantee().value(), type_map);
}

class SeriesRuntimeType final : public hg::TimeSeriesRuntimeType {
  public:
    explicit SeriesRuntimeType(const hg::TimeSeriesType &type)
        : TimeSeriesRuntimeType{type, sizeof(hg::i64), alignof(hg::i64), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<hg::i64 *>(destination));
    }

    void destroy_value(void *value, hg::allocator &) const noexcept override {
        std::destroy_at(static_cast<hg::i64 *>(value));
    }
};

struct SeriesViewStorage {
    static constexpr hg::i64 guard_value = -1;

    hg::i64 value{};
    hg::i64 guard{guard_value};
};

class SeriesViewRuntimeType final : public hg::TimeSeriesRuntimeType {
  public:
    explicit SeriesViewRuntimeType(const hg::TimeSeriesType &type)
        : TimeSeriesRuntimeType{type, sizeof(SeriesViewStorage), alignof(SeriesViewStorage), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<SeriesViewStorage *>(destination));
    }

    void destroy_value(void *value, hg::allocator &) const noexcept override {
        std::destroy_at(static_cast<SeriesViewStorage *>(value));
    }
};

const hg::TimeSeriesRuntimeType &series_runtime_type(const hg::TimeSeriesType &type) {
    static const auto ordinary = SeriesRuntimeType{series_type(hg::TimeSeriesGuarantee::ordinary)};
    static const auto deduplicated = SeriesRuntimeType{series_type(hg::TimeSeriesGuarantee::deduplicated)};
    static const auto final = SeriesRuntimeType{series_type(hg::TimeSeriesGuarantee::final)};
    static const auto constant = SeriesRuntimeType{series_type(hg::TimeSeriesGuarantee::const_)};

    switch (*type.guarantee().value()) {
    case hg::TimeSeriesGuarantee::ordinary:
        return ordinary;
    case hg::TimeSeriesGuarantee::deduplicated:
        return deduplicated;
    case hg::TimeSeriesGuarantee::final:
        return final;
    case hg::TimeSeriesGuarantee::const_:
        return constant;
    }

    std::unreachable();
}

const hg::TimeSeriesRuntimeType &series_view_runtime_type(const hg::TimeSeriesType &type) {
    static const auto ordinary = SeriesViewRuntimeType{series_type(hg::TimeSeriesGuarantee::ordinary)};
    static const auto deduplicated = SeriesViewRuntimeType{series_type(hg::TimeSeriesGuarantee::deduplicated)};
    static const auto final = SeriesViewRuntimeType{series_type(hg::TimeSeriesGuarantee::final)};
    static const auto constant = SeriesViewRuntimeType{series_type(hg::TimeSeriesGuarantee::const_)};

    switch (*type.guarantee().value()) {
    case hg::TimeSeriesGuarantee::ordinary:
        return ordinary;
    case hg::TimeSeriesGuarantee::deduplicated:
        return deduplicated;
    case hg::TimeSeriesGuarantee::final:
        return final;
    case hg::TimeSeriesGuarantee::const_:
        return constant;
    }

    std::unreachable();
}

} // namespace provider

namespace hg {

template <auto... Properties>
struct time_series_type_traits<provider::Series<i64, Properties...>> {
    using semantic_type = provider::SeriesType;

    static constexpr bool generic = false;
    static constexpr bool supported = true;

    template <typename, std::size_t, TimeSeriesTypeRole>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        if (!guarantee.is_resolved()) {
            static const auto type = provider::SeriesType{guarantee};
            [[maybe_unused]] static const auto &registered_type = TypeRegistry::register_static(type);
            return registered_type;
        }
        return provider::series_type(*guarantee.value());
    }
};

template <auto... Properties>
struct time_series_runtime_traits<provider::Series<i64, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return provider::series_runtime_type(type);
    }
};

template <auto... Properties>
struct output_declaration_traits<Out<provider::Series<i64, Properties...>, provider::view>> {
    using time_series_type = provider::Series<i64, Properties...>;

    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return provider::series_view_runtime_type(type);
    }
};

template <auto... SeriesProperties, auto... Properties>
class In<provider::Series<i64, SeriesProperties...>, Properties...>
    : public InputActivityControl<InputProperties<Properties...>::dynamic_activity> {
    using ParsedProperties = InputProperties<Properties...>;
    using ActivityControl = InputActivityControl<ParsedProperties::dynamic_activity>;

  public:
    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : ActivityControl{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    i64 value() const noexcept { return *static_cast<const i64 *>(data_.data()); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }

    static constexpr auto name = ParsedProperties::name;
    static constexpr auto activity = ParsedProperties::activity;
    static constexpr auto validity = ParsedProperties::validity;
    static constexpr auto dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr auto dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    TimeSeriesInputData data_;
};

template <auto... Properties>
class Out<provider::Series<i64, Properties...>> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    void set(i64 value) const {
        *static_cast<i64 *>(binding_->mutable_data()) = value;
        binding_->publish();
    }

    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    using time_series_type = provider::Series<i64, Properties...>;

  private:
    TimeSeriesOutputBinding binding_;
};

template <auto... Properties>
class Out<provider::Series<i64, Properties...>, provider::view> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    void set(i64 value) const {
        *static_cast<i64 *>(binding_->mutable_data()) = value;
        binding_->publish();
    }

    void invalidate() const {
        if (provider::view_invalidation_observer != nullptr)
            *provider::view_invalidation_observer = true;
        binding_->invalidate();
    }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    using time_series_type = provider::Series<i64, Properties...>;

  private:
    TimeSeriesOutputBinding binding_;
};

} // namespace hg

namespace provider {

struct ViewAggregate {
    hg::In<hg::TS<hg::i64>> input;
    hg::Out<Series<hg::i64>, provider::view> output;

    void evaluate() const { output.set(*input); }
};

struct DefaultAggregate {
    hg::In<Series<hg::i64>> input;
    hg::Out<Series<hg::i64>> output;

    void evaluate() const { output.set(input.value()); }
};

} // namespace provider

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

using Series = provider::Series<hg::i64>;

void test_provider_descriptor_and_views() {
    const auto &semantic_type = Series::type();
    const auto &default_runtime = hg::cpp_time_series_runtime_type<Series>();
    const auto &view_runtime = provider::series_view_runtime_type(semantic_type);
    hg::register_time_series_runtime_type(default_runtime);

    require(&default_runtime.type() == &semantic_type, "provider runtime has the wrong semantic type");
    require(&hg::resolve_time_series_runtime_type(semantic_type) == &default_runtime,
            "provider runtime was not selected as the default");
    require(&view_runtime != &default_runtime && view_runtime.value_size() != default_runtime.value_size(),
            "provider view runtime is not distinct from the default runtime");

    const auto updated = hg::MIN_DT + std::chrono::microseconds{1};
    const auto invalidated = updated + std::chrono::microseconds{1};
    auto input_value = hg::i64{17};
    auto input_state = hg::TimeSeriesState{};
    input_state.publish(updated);
    const auto input_data = hg::TimeSeriesInputData{default_runtime, input_state, &input_value, true};
    const auto input = hg::In<Series>{input_data};
    require(input.valid() && input.became_valid() && !input.became_invalid() && input.modified() && input.value() == 17,
            "provider input view did not expose its valid state");

    input_state.invalidate(invalidated);
    const auto invalidated_input_data = hg::TimeSeriesInputData{default_runtime, input_state, &input_value, true};
    const auto invalidated_input = hg::In<Series>{invalidated_input_data};
    require(!invalidated_input.valid() && !invalidated_input.became_valid() && invalidated_input.became_invalid() &&
                invalidated_input.last_updated() == invalidated,
            "provider input view did not expose its invalid state");

    auto output_time = hg::TimeSeriesState{};
    auto output_value = provider::SeriesViewStorage{};
    auto output_state = hg::OutputState{view_runtime};
    output_state.bind(output_time, &output_value, updated);
    const auto output = hg::Out<Series, provider::view>{hg::TimeSeriesOutputBinding{output_state}};
    output.set(23);
    require(output_time.valid() && output_time.became_valid() && output_time.time() == updated && output_value.value == 23 &&
                output_value.guard == provider::SeriesViewStorage::guard_value,
            "provider view output did not publish into its selected runtime storage");

    const auto scalar = hg::ScalarData{hg::cpp_scalar_runtime_type<hg::i64>(), &output_value.value};
    require(scalar.mode() == hg::ScalarDataMode::stored_view && scalar.data() == &output_value.value,
            "public scalar descriptor did not preserve a stored view");

    const auto erased_input = hg::In<hg::TS_T>{input_data};
    auto input_fallback = false;
    erased_input.visit(
        [&](const hg::In<hg::TS_T> *value) { input_fallback = &value->runtime_type() == &default_runtime; }, ddv::noop);
    require(input_fallback, "erased input did not select the exact fallback view");

    const auto erased_output = hg::Out<hg::TS_T>{hg::TimeSeriesOutputBinding{output_state}};
    auto output_fallback = false;
    erased_output.visit(
        [&](const hg::Out<hg::TS_T> *value) { output_fallback = &value->runtime_type() == &view_runtime; },
        ddv::noop);
    require(output_fallback, "erased output did not select the exact fallback view");

    auto output_invalidated = false;
    provider::view_invalidation_observer = &output_invalidated;
    output.invalidate();
    provider::view_invalidation_observer = nullptr;
    require(output_invalidated && !output_time.valid() && output_time.became_invalid() && output_time.time() == updated,
            "provider view output did not invalidate through its selected specification");
}

void test_provider_output_specifications() {
    const auto &default_runtime = hg::cpp_time_series_runtime_type<Series>();
    const auto &view_runtime = provider::series_view_runtime_type(Series::type());

    const auto callable_view = hg::node([](hg::In<hg::TS<hg::i64>> input, hg::Out<Series, provider::view> output) {
        output.set(*input);
    });
    const auto callable_default = hg::node([](hg::In<Series> input, hg::Out<Series> output) {
        output.set(input.value());
    });
    const auto *callable_runtime = static_cast<const hg::TimeSeriesRuntimeType *>(nullptr);
    const auto callable_consume = hg::node([&](hg::In<Series> input, hg::Out<hg::TS<hg::i64>> output) {
        callable_runtime = &input.runtime_type();
        output.set(input.value());
    });
    const auto callable_graph = [&](hg::wp<hg::TS<hg::i64>> input) {
        return callable_consume(callable_default(callable_view(input)));
    };
    require(hg::run_graph(callable_graph, std::array<hg::i64, 2>{3, 8}).equals<hg::i64>({3, 8}) &&
                callable_runtime == &default_runtime,
            "callable default output selected its input view runtime");

    const auto view_aggregate = hg::node<provider::ViewAggregate>();
    const auto *view_aggregate_runtime = static_cast<const hg::TimeSeriesRuntimeType *>(nullptr);
    const auto view_aggregate_consume = hg::node([&](hg::In<Series> input, hg::Out<hg::TS<hg::i64>> output) {
        view_aggregate_runtime = &input.runtime_type();
        output.set(input.value());
    });
    const auto view_aggregate_graph = [&](hg::wp<hg::TS<hg::i64>> input) {
        return view_aggregate_consume(view_aggregate(input));
    };
    require(hg::run_graph(view_aggregate_graph, std::array<hg::i64, 2>{5, 7}).equals<hg::i64>({5, 7}) &&
                view_aggregate_runtime == &view_runtime,
            "aggregate view output did not select its view runtime");

    const auto default_aggregate = hg::node<provider::DefaultAggregate>();
    const auto *default_aggregate_runtime = static_cast<const hg::TimeSeriesRuntimeType *>(nullptr);
    const auto default_aggregate_consume = hg::node([&](hg::In<Series> input, hg::Out<hg::TS<hg::i64>> output) {
        default_aggregate_runtime = &input.runtime_type();
        output.set(input.value());
    });
    const auto default_aggregate_graph = [&](hg::wp<hg::TS<hg::i64>> input) {
        return default_aggregate_consume(default_aggregate(callable_view(input)));
    };
    require(hg::run_graph(default_aggregate_graph, std::array<hg::i64, 2>{11, 14}).equals<hg::i64>({11, 14}) &&
                default_aggregate_runtime == &default_runtime,
            "aggregate default output selected its input view runtime");
}

void test_view_output_invalidation() {
    auto invalidated = false;
    provider::view_invalidation_observer = &invalidated;

    const auto source = hg::node([](hg::In<hg::TS<hg::i64>> input, hg::Out<hg::TS<hg::i64>> output) {
        if (*input == 0) {
            output.invalidate();
            return;
        }
        output.set(*input);
    });
    const auto provide = hg::node<provider::ViewAggregate>();
    const auto consume = hg::node([](hg::In<Series> input, hg::Out<hg::TS<hg::i64>> output) {
        output.set(input.value());
    });
    const auto graph = [&](hg::wp<hg::TS<hg::i64>> input) { return consume(provide(source(input))); };

    const auto result = hg::run_graph(graph, std::array<hg::i64, 2>{12, 0});
    provider::view_invalidation_observer = nullptr;
    require(result.equals<hg::i64>({12, std::nullopt}) && invalidated,
            "view output invalidation did not use the provider view specification");
}

} // namespace

int main() {
    try {
        test_provider_descriptor_and_views();
        test_provider_output_specifications();
        test_view_output_invalidation();
        std::cout << "hg_frame_provider_integration_test: external provider integration passed\n";
    } catch (const std::exception &error) {
        std::cerr << "hg_frame_provider_integration_test: " << error.what() << '\n';
        return 1;
    }
}
