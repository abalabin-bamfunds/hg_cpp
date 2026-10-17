#pragma once

#include <bg/queue.hpp>
#include <bg/node/details/callable_node.hpp>
#include <bg/node/details/callable_signature.hpp>
#include <bg/node/output_declaration_traits.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/queue_mode.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/util/reflection.hpp>
#include <bg/util/named_argument.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

struct NoQueueCallback {};

template <typename Function, auto... Selectors> struct QueueCallback {
    Function function;
};

template <typename Callback> struct QueueStartOption {
    Callback callback;
};

template <typename Callback> struct QueueStopOption {
    Callback callback;
};

struct QueueModeOption {
    QueueMode value;
};

struct QueueOutputTypeOption {
    const TimeSeriesType *type;
};

struct QueueStartName {
    template <typename Function> constexpr auto operator=(Function &&function) const {
        using Callback = QueueCallback<std::decay_t<Function>>;
        return QueueStartOption<Callback>{Callback{std::forward<Function>(function)}};
    }

    template <auto... Selectors, typename Function> constexpr auto bind(Function &&function) const {
        using Callback = QueueCallback<std::decay_t<Function>, Selectors...>;
        return QueueStartOption<Callback>{Callback{std::forward<Function>(function)}};
    }
};

struct QueueStopName {
    template <typename Function> constexpr auto operator=(Function &&function) const {
        using Callback = QueueCallback<std::decay_t<Function>>;
        return QueueStopOption<Callback>{Callback{std::forward<Function>(function)}};
    }

    template <auto... Selectors, typename Function> constexpr auto bind(Function &&function) const {
        using Callback = QueueCallback<std::decay_t<Function>, Selectors...>;
        return QueueStopOption<Callback>{Callback{std::forward<Function>(function)}};
    }
};

struct QueueModeName {
    constexpr auto operator=(QueueMode value) const noexcept { return QueueModeOption{value}; }
};

struct QueueOutputTypeName {
    constexpr auto operator=(const TimeSeriesType &type) const noexcept { return QueueOutputTypeOption{&type}; }
};

template <typename T> struct queue_start_option : std::false_type {
    using function_type = void;
};
template <typename Function> struct queue_start_option<QueueStartOption<Function>> : std::true_type {
    using function_type = Function;
};

template <typename T> struct queue_stop_option : std::false_type {
    using function_type = void;
};
template <typename Function> struct queue_stop_option<QueueStopOption<Function>> : std::true_type {
    using function_type = Function;
};

template <typename Callback> struct queue_callback_traits;
template <typename Function, auto... Selectors>
struct queue_callback_traits<QueueCallback<Function, Selectors...>> {
    using function_type = Function;
    static constexpr auto selector_count = sizeof...(Selectors);
    static constexpr bool has_selectors = selector_count != 0;

    template <std::size_t Index> static consteval auto selector() {
        static_assert(Index < sizeof...(Selectors));
        return std::get<Index>(std::tuple{std::integral_constant<decltype(Selectors), Selectors>{}...}).value;
    }
};

template <> struct queue_callback_traits<NoQueueCallback> {
    using function_type = NoQueueCallback;
    static constexpr auto selector_count = std::size_t{};
    static constexpr bool has_selectors = false;
};

template <typename Callback> using queue_callback_function_t = typename queue_callback_traits<Callback>::function_type;

template <typename T> inline constexpr bool queue_mode_option = std::same_as<T, QueueModeOption>;
template <typename T> inline constexpr bool queue_output_type_option = std::same_as<T, QueueOutputTypeOption>;

template <template <typename> typename Match, typename Default, typename... Options> struct queue_option_type {
    using type = Default;
};

template <template <typename> typename Match, typename Default, typename First, typename... Rest>
struct queue_option_type<Match, Default, First, Rest...> {
    using type = std::conditional_t<Match<First>::value, typename Match<First>::function_type,
                                    typename queue_option_type<Match, Default, Rest...>::type>;
};

template <typename... Options> struct queue_option_types {
    using start_type = typename queue_option_type<queue_start_option, NoQueueCallback, Options...>::type;
    using stop_type = typename queue_option_type<queue_stop_option, NoQueueCallback, Options...>::type;
    static constexpr auto start_count = (std::size_t{} + ... + std::size_t{queue_start_option<Options>::value});
    static constexpr auto stop_count = (std::size_t{} + ... + std::size_t{queue_stop_option<Options>::value});
    static constexpr auto mode_count = (std::size_t{} + ... + std::size_t{queue_mode_option<Options>});
    static constexpr auto output_count = (std::size_t{} + ... + std::size_t{queue_output_type_option<Options>});
    static constexpr bool supported =
        ((queue_start_option<Options>::value || queue_stop_option<Options>::value || queue_mode_option<Options> ||
          queue_output_type_option<Options>) && ...);
};

template <typename Callback> struct queue_callback_indices {
    using Function = queue_callback_function_t<Callback>;
    using type = std::make_index_sequence<callable<Function>::argument_count>;
};

template <> struct queue_callback_indices<NoQueueCallback> {
    using type = std::index_sequence<>;
};

template <typename Function, typename... Options>
using queue_callback_indices_t = typename queue_callback_indices<Function, Options...>::type;

template <typename Wrapper, typename First, typename... Rest>
decltype(auto) queue_callback_value(First &&first, Rest &&...rest) {
    if constexpr (std::same_as<std::remove_cvref_t<First>, Wrapper>) {
        return std::forward<First>(first).callback;
    } else {
        static_assert(sizeof...(Rest) != 0, "queue callback option is missing");
        return queue_callback_value<Wrapper>(std::forward<Rest>(rest)...);
    }
}

template <typename Wrapper> auto queue_callback_value() {
    return NoQueueCallback{};
}

template <typename Function, std::size_t... Index>
consteval std::size_t queue_parameter_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            std::size_t{std::same_as<std::remove_cvref_t<typename callable<Function>::template argument_type<Index>>,
                                     Queue>});
}

template <typename Function, std::size_t Index>
inline constexpr bool queue_callback_parameter =
    std::same_as<std::remove_cvref_t<typename callable<Function>::template argument_type<Index>>, Queue>;

template <typename Function, std::size_t Index>
inline constexpr bool queue_parameter_is_by_value =
    queue_callback_parameter<Function, Index> &&
    !std::is_reference_v<typename callable<Function>::template argument_type<Index>>;

template <typename Function, std::size_t... Index>
consteval bool queue_parameters_supported(std::index_sequence<Index...>) {
    return ((queue_callback_parameter<Function, Index> || scalar_field<callable_field<Function, Index>>::value) && ...);
}

template <typename Function, std::size_t... Index>
consteval bool queue_parameters_pass_queue_by_value(std::index_sequence<Index...>) {
    return ((!queue_callback_parameter<Function, Index> || queue_parameter_is_by_value<Function, Index>) && ...);
}

template <typename Function, std::size_t... Index>
consteval bool queue_callback_is_const_invocable(std::index_sequence<Index...>) {
    return std::is_invocable_v<const Function &,
                               typename callable<Function>::template argument_type<Index>...>;
}

template <typename Function, std::size_t Target, std::size_t... Index>
consteval std::size_t queue_scalar_parameter_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... + std::size_t{Index < Target && !queue_callback_parameter<Function, Index>});
}

template <typename Function, std::size_t ScalarIndex, std::size_t ArgumentIndex = 0, std::size_t Seen = 0>
consteval std::size_t queue_scalar_argument_index() {
    if constexpr (ArgumentIndex == callable<Function>::argument_count) {
        return ArgumentIndex;
    } else if constexpr (queue_callback_parameter<Function, ArgumentIndex>) {
        return queue_scalar_argument_index<Function, ScalarIndex, ArgumentIndex + 1, Seen>();
    } else if constexpr (Seen == ScalarIndex) {
        return ArgumentIndex;
    } else {
        return queue_scalar_argument_index<Function, ScalarIndex, ArgumentIndex + 1, Seen + 1>();
    }
}

template <typename Function, std::size_t... Index>
consteval std::size_t queue_scalar_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... + std::size_t{!queue_callback_parameter<Function, Index>});
}

template <typename Callback, std::size_t... Index>
consteval bool queue_callback_names_are_valid(std::index_sequence<Index...>) {
    if constexpr (!queue_callback_traits<Callback>::has_selectors) {
        return true;
    } else {
        return ((is_compile_time_name<std::remove_cv_t<decltype(
                     queue_callback_traits<Callback>::template selector<Index>())>>::value) &&
                ...);
    }
}

template <typename Callback, std::size_t Index, std::size_t... Other>
consteval bool queue_start_name_is_unique(std::index_sequence<Other...>) {
    return ((Index == Other || queue_callback_traits<Callback>::template selector<Index>().text !=
                                  queue_callback_traits<Callback>::template selector<Other>().text) && ...);
}

template <typename Callback, std::size_t... Index>
consteval bool queue_start_names_are_unique(std::index_sequence<Index...> indices) {
    return (queue_start_name_is_unique<Callback, Index>(indices) && ...);
}

template <typename Callback, std::size_t... Index>
consteval bool queue_start_selectors_are_valid(std::index_sequence<Index...>) {
    using Function = queue_callback_function_t<Callback>;
    constexpr auto ScalarCount = queue_scalar_count<Function>(
        std::make_index_sequence<callable<Function>::argument_count>{});
    if constexpr (!queue_callback_traits<Callback>::has_selectors) {
        return true;
    } else if constexpr (queue_callback_traits<Callback>::selector_count != ScalarCount) {
        return false;
    } else if constexpr (!queue_callback_names_are_valid<Callback>(std::make_index_sequence<ScalarCount>{})) {
        return false;
    } else {
        return queue_start_names_are_unique<Callback>(std::make_index_sequence<ScalarCount>{});
    }
}

template <typename Callback, std::size_t... Index>
consteval bool queue_stop_selectors_are_valid(std::index_sequence<Index...>) {
    using Function = queue_callback_function_t<Callback>;
    constexpr auto ScalarCount = queue_scalar_count<Function>(
        std::make_index_sequence<callable<Function>::argument_count>{});
    if constexpr (!queue_callback_traits<Callback>::has_selectors) {
        return true;
    } else {
        return queue_callback_traits<Callback>::selector_count == ScalarCount &&
               ((is_compile_time_name<std::remove_cv_t<decltype(
                     queue_callback_traits<Callback>::template selector<Index>())>>::value ||
                 std::integral<std::remove_cv_t<decltype(
                     queue_callback_traits<Callback>::template selector<Index>())>>) && ...);
    }
}

template <typename StartCallback, typename StopCallback, std::size_t StopScalarIndex,
          std::size_t SourceScalarIndex>
consteval bool queue_stop_scalar_types_match() {
    using StartFunction = queue_callback_function_t<StartCallback>;
    using StopFunction = queue_callback_function_t<StopCallback>;
    constexpr auto StopArgument = queue_scalar_argument_index<StopFunction, StopScalarIndex>();
    constexpr auto SourceArgument = queue_scalar_argument_index<StartFunction, SourceScalarIndex>();
    using StopField = callable_field<StopFunction, StopArgument>;
    using SourceField = callable_field<StartFunction, SourceArgument>;
    using SourceType = canonical_scalar_type_t<typename scalar_field<SourceField>::parameter_type>;
    using StopType = canonical_scalar_type_t<typename scalar_field<StopField>::parameter_type>;
    return std::same_as<SourceType, StopType>;
}

template <typename StartCallback, typename StopCallback, std::size_t StopScalarIndex, std::size_t... SourceScalarIndex>
consteval auto queue_stop_scalar_type_matches(std::index_sequence<SourceScalarIndex...>) {
    return std::array<bool, sizeof...(SourceScalarIndex)>{
        queue_stop_scalar_types_match<StartCallback, StopCallback, StopScalarIndex, SourceScalarIndex>()...};
}

template <auto Selector, typename StartCallback, std::size_t... Index>
consteval std::size_t queue_named_start_scalar_index(std::index_sequence<Index...>) {
    using SelectorType = std::remove_cv_t<decltype(Selector)>;
    constexpr auto MatchCount = (std::size_t{} + ... + std::size_t{
        (queue_callback_traits<StartCallback>::template selector<Index>().text == SelectorType::text)});
    static_assert(MatchCount == 1, "a named stop binding must identify exactly one start scalar parameter");
    auto result = std::size_t{};
    ((queue_callback_traits<StartCallback>::template selector<Index>().text == SelectorType::text
          ? result = Index
          : result), ...);
    return result;
}

template <typename StartCallback, typename StopCallback, std::size_t StopScalarIndex>
consteval std::size_t queue_stop_source_scalar_index() {
    using StartFunction = queue_callback_function_t<StartCallback>;
    using StopFunction = queue_callback_function_t<StopCallback>;
    using StopTraits = queue_callback_traits<StopCallback>;
    constexpr auto StartScalars = queue_scalar_count<StartFunction>(
        std::make_index_sequence<callable<StartFunction>::argument_count>{});
    constexpr auto StopArgument = queue_scalar_argument_index<StopFunction, StopScalarIndex>();

    constexpr auto SourceIndex = []() consteval {
        if constexpr (StopTraits::has_selectors) {
            constexpr auto Selector = StopTraits::template selector<StopScalarIndex>();
            if constexpr (is_compile_time_name<std::remove_cv_t<decltype(Selector)>>::value) {
                static_assert(queue_callback_traits<StartCallback>::has_selectors,
                              "a named stop binding requires named start scalar parameters");
                if constexpr (queue_callback_traits<StartCallback>::has_selectors) {
                    return queue_named_start_scalar_index<Selector, StartCallback>(
                        std::make_index_sequence<StartScalars>{});
                } else {
                    return std::size_t{};
                }
            } else {
                static_assert(std::integral<std::remove_cv_t<decltype(Selector)>>,
                              "a stop binding selector must be a scalar index or name");
                constexpr auto Index = static_cast<std::size_t>(Selector);
                static_assert(Index < StartScalars, "a stop binding scalar index is out of range");
                return Index;
            }
        } else {
            constexpr auto TypeMatches = queue_stop_scalar_type_matches<StartCallback, StopCallback, StopScalarIndex>(
                std::make_index_sequence<StartScalars>{});
            constexpr auto MatchCount = std::ranges::count(TypeMatches, true);
            static_assert(MatchCount == 1,
                          "a stop scalar must match exactly one start scalar; use stop.bind<index or name>()");
            return static_cast<std::size_t>(std::ranges::find(TypeMatches, true) - TypeMatches.begin());
        }
    }();

    static_assert(SourceIndex < StartScalars, "a stop scalar binding must select an existing start scalar");
    if constexpr (SourceIndex < StartScalars) {
        constexpr auto SourceArgument = queue_scalar_argument_index<StartFunction, SourceIndex>();
        using SourceField = callable_field<StartFunction, SourceArgument>;
        using StopField = callable_field<StopFunction, StopArgument>;
        using SourceType = canonical_scalar_type_t<typename scalar_field<SourceField>::parameter_type>;
        using StopType = canonical_scalar_type_t<typename scalar_field<StopField>::parameter_type>;
        static_assert(std::same_as<SourceType, StopType>, "a stop binding must accept the selected start scalar type");
        static_assert(!scalar_field<SourceField>::optional || scalar_field<StopField>::optional,
                      "an optional start scalar requires an optional stop scalar");
    }
    return SourceIndex;
}

template <typename Function, std::size_t Index, typename Invocation>
auto make_queue_callback_argument(Invocation invocation, Queue &queue) {
    using Field = callable_field<Function, Index>;
    if constexpr (queue_callback_parameter<Function, Index>) {
        return std::move(queue);
    } else {
        constexpr auto ParameterIndex = queue_scalar_parameter_index<
            Function, Index>(std::make_index_sequence<callable<Function>::argument_count>{});
        return make_callable_scalar_argument<Field>(invocation, ParameterIndex);
    }
}

template <typename Callback, typename Invocation, std::size_t... Index>
void invoke_queue_start(const Callback &callback, Invocation invocation, Queue &queue,
                        std::index_sequence<Index...>) {
    using Function = queue_callback_function_t<Callback>;
    std::invoke(callback.function, make_queue_callback_argument<Function, Index>(invocation, queue)...);
}

template <typename StartCallback, typename StopCallback, typename Invocation, std::size_t... Index>
void invoke_queue_stop(const StopCallback &callback, Invocation invocation, std::size_t node_index,
                       std::index_sequence<Index...>) noexcept {
    using Function = queue_callback_function_t<StopCallback>;
    auto &runtime_graph = invocation.context().runtime_graph();
    std::invoke(callback.function,
                ([&] {
                    if constexpr (queue_callback_parameter<Function, Index>) {
                        return runtime_graph.make_queue(node_index);
                    } else {
                        constexpr auto StopScalarIndex = queue_scalar_parameter_index<
                            Function, Index>(std::make_index_sequence<callable<Function>::argument_count>{});
                        constexpr auto SourceScalarIndex =
                            queue_stop_source_scalar_index<StartCallback, StopCallback, StopScalarIndex>();
                        return make_callable_scalar_argument<callable_field<Function, Index>>(
                            invocation, SourceScalarIndex);
                    }
                }())...);
}

template <typename Start, typename Stop> struct QueueNodeCallbacks {
    Start start;
    Stop stop;
};

template <typename Output, bool RuntimeOutput> struct QueueNodeOutputType {
    using output_time_series_type = Output;
};

template <typename Output> struct QueueNodeOutputType<Output, true> {};

template <typename Output, typename Start, typename Stop, bool RuntimeOutput>
class QueueNodeDefinition : public QueueNodeOutputType<Output, RuntimeOutput> {
    using StartFunction = queue_callback_function_t<Start>;
    using StopFunction = queue_callback_function_t<Stop>;
    using StartParameters = queue_callback_indices_t<Start>;
    static constexpr bool has_stop = !std::same_as<Stop, NoQueueCallback>;
    using StopParameters = queue_callback_indices_t<Stop>;

  public:
    using compiled_data_type = QueueNodeCallbacks<Start, Stop>;

    QueueNodeDefinition(Start start, Stop stop, QueueMode mode, const TimeSeriesType *runtime_output)
        : start_{std::move(start)}, stop_{std::move(stop)}, mode_{mode},
          signature_{make_signature(checked_runtime_output(runtime_output))} {
        static_assert(std::copy_constructible<StartFunction>, "a queue start callback must be copy constructible");
        static_assert(std::same_as<typename callable<StartFunction>::result_type, void>,
                      "a queue start callback must return void");
        static_assert(queue_callback_is_const_invocable<StartFunction>(StartParameters{}),
                      "a queue start callback must be const-invocable");
        static_assert(queue_parameter_count<StartFunction>(StartParameters{}) == 1,
                      "a queue start callback must take exactly one Queue parameter by value");
        static_assert(queue_parameters_pass_queue_by_value<StartFunction>(StartParameters{}),
                      "a queue callback must take Queue by value");
        static_assert(queue_parameters_supported<StartFunction>(StartParameters{}),
                      "queue callbacks accept only Queue and scalar arguments");
        static_assert(queue_start_selectors_are_valid<Start>(
                          std::make_index_sequence<queue_callback_traits<Start>::selector_count>{}),
                      "start.bind requires one unique name for each start scalar argument");
        if constexpr (has_stop) {
            static_assert(std::copy_constructible<StopFunction>, "a queue stop callback must be copy constructible");
            static_assert(queue_parameters_pass_queue_by_value<StopFunction>(StopParameters{}),
                          "a queue callback must take Queue by value");
            static_assert(queue_parameter_count<StopFunction>(StopParameters{}) <= 1,
                          "a queue stop callback may take at most one Queue parameter");
            static_assert(queue_parameters_supported<StopFunction>(StopParameters{}),
                          "queue callbacks accept only Queue and scalar arguments");
            static_assert(queue_stop_selectors_are_valid<Stop>(
                              std::make_index_sequence<queue_callback_traits<Stop>::selector_count>{}),
                          "stop.bind requires one scalar selector for each stop scalar argument");
            static_assert(std::same_as<typename callable<StopFunction>::result_type, void>,
                          "a queue stop callback must return void");
            static_assert(noexcept_invoke_stop(std::make_index_sequence<callable<StopFunction>::argument_count>{}),
                          "a queue stop callback must be noexcept");
        }
    }

    const Signature &signature() const noexcept { return signature_; }
    static constexpr bool uses_queue = true;
    QueueMode queue_mode() const noexcept { return mode_; }

    const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &arguments) const {
        const auto &runtime_type = [&]() -> const TimeSeriesRuntimeType & {
            if constexpr (RuntimeOutput) {
                return resolve_time_series_runtime_type(*arguments.resolved_output_type());
            } else {
                return output_declaration_traits<Out<Output>>::runtime(*arguments.resolved_output_type());
            }
        }();
        validate_queue_output_type(runtime_type.type(), mode_);
        return runtime_type;
    }

    compiled_data_type compile(const ResolvedSignature &, allocator &) const { return {start_, stop_}; }

    static void start(NodeInvocation<compiled_data_type> invocation) {
        auto queue = invocation.context().runtime_graph().make_queue(invocation.context().node_index());
        invoke_queue_start(invocation.compiled_data().start, invocation, queue, StartParameters{});
    }

    static void stop(NodeInvocation<compiled_data_type> invocation) noexcept
        requires has_stop
    {
        invoke_queue_stop<Start>(invocation.compiled_data().stop, invocation, invocation.context().node_index(),
                                 StopParameters{});
    }

  private:
    static const TimeSeriesType *checked_runtime_output(const TimeSeriesType *type) {
        if constexpr (RuntimeOutput) {
            if (type == nullptr) {
                throw std::invalid_argument{"a runtime queue node requires output_type"};
            }
        }
        return type;
    }

    Signature make_signature(const TimeSeriesType *runtime_output) const {
        auto parameters = std::vector<Parameter>{};
        parameters.reserve(queue_scalar_count<StartFunction>(StartParameters{}));
        append_scalar_parameters(parameters, StartParameters{});
        if constexpr (RuntimeOutput) {
            return Signature{util::symbol{util::unqualified_type_name<QueueNodeDefinition>()}, std::move(parameters),
                             *runtime_output};
        } else {
            return Signature{util::symbol{util::unqualified_type_name<QueueNodeDefinition>()}, std::move(parameters),
                             callable_output_type<QueueNodeDefinition, Output, 0>()};
        }
    }

    template <std::size_t... Index> static void append_scalar_parameters(std::vector<Parameter> &parameters,
                                                                           std::index_sequence<Index...>) {
        ([&] {
            using Field = callable_field<StartFunction, Index>;
            if constexpr (!queue_callback_parameter<StartFunction, Index>) {
                using Value = typename scalar_field<Field>::parameter_type;
                auto name = std::to_string(parameters.size());
                if constexpr (queue_callback_traits<Start>::has_selectors) {
                    name = std::string{queue_callback_traits<Start>::template selector<
                        queue_scalar_parameter_index<StartFunction, Index>(
                            std::make_index_sequence<callable<StartFunction>::argument_count>{})>().text.view()};
                }
                parameters.push_back({util::symbol{name}, &scalar_type_for<Value>(),
                                      active, required, scalar_field<Field>::optional});
            }
        }(),
         ...);
    }

    template <std::size_t... Index> static consteval bool noexcept_invoke_stop(std::index_sequence<Index...>) {
        return std::is_nothrow_invocable_v<const StopFunction &,
                                           typename callable<StopFunction>::template argument_type<Index>...>;
    }

    Start start_;
    Stop stop_;
    QueueMode mode_;
    Signature signature_;
};

template <typename... Options> inline constexpr bool valid_queue_options = queue_option_types<Options...>::supported &&
    queue_option_types<Options...>::start_count == 1 && queue_option_types<Options...>::stop_count <= 1 &&
    queue_option_types<Options...>::mode_count <= 1 && queue_option_types<Options...>::output_count <= 1;

template <typename... Options> auto make_queue_callbacks(Options &&...options) {
    using Types = queue_option_types<std::remove_cvref_t<Options>...>;
    using Start = typename Types::start_type;
    using Stop = typename Types::stop_type;
    auto start = [&] {
        if constexpr (std::same_as<Start, NoQueueCallback>) {
            return Start{};
        } else {
            return queue_callback_value<QueueStartOption<Start>>(std::forward<Options>(options)...);
        }
    }();
    auto stop = [&] {
        if constexpr (std::same_as<Stop, NoQueueCallback>) {
            return Stop{};
        } else {
            return queue_callback_value<QueueStopOption<Stop>>(std::forward<Options>(options)...);
        }
    }();
    auto mode = QueueMode::standard;
    auto output = static_cast<const TimeSeriesType *>(nullptr);
    ([&] {
        using Option = std::remove_cvref_t<Options>;
        if constexpr (queue_mode_option<Option>) {
            mode = options.value;
        } else if constexpr (queue_output_type_option<Option>) {
            output = options.type;
        }
    }(), ...);
    return std::tuple{std::move(start), std::move(stop), mode, output};
}

} // namespace bg::details

namespace bg {

inline constexpr auto start = details::QueueStartName{};
inline constexpr auto stop = details::QueueStopName{};
inline constexpr auto mode = details::QueueModeName{};
inline constexpr auto output_type = details::QueueOutputTypeName{};

template <typename Output, typename... Options>
    requires details::supported_time_series<Output> && details::valid_queue_options<std::remove_cvref_t<Options>...> &&
             (details::queue_option_types<std::remove_cvref_t<Options>...>::output_count == 0)
[[nodiscard]] auto queue(Options &&...options) {
    auto [start_callback, stop_callback, mode_value, output] =
        details::make_queue_callbacks(std::forward<Options>(options)...);
    if (mode_value != QueueMode::standard && mode_value != QueueMode::elide && mode_value != QueueMode::batch) {
        throw std::invalid_argument{"queue mode is unsupported"};
    }
    using Definition = details::QueueNodeDefinition<Output, decltype(start_callback), decltype(stop_callback), false>;
    return RuntimeSignatureNodeTemplate<Definition>{Definition{
        std::move(start_callback), std::move(stop_callback), mode_value, output}};
}

template <typename... Options>
    requires details::valid_queue_options<std::remove_cvref_t<Options>...> &&
             (details::queue_option_types<std::remove_cvref_t<Options>...>::output_count == 1)
[[nodiscard]] auto queue(Options &&...options) {
    auto [start_callback, stop_callback, mode_value, output] =
        details::make_queue_callbacks(std::forward<Options>(options)...);
    if (mode_value != QueueMode::standard && mode_value != QueueMode::elide && mode_value != QueueMode::batch) {
        throw std::invalid_argument{"queue mode is unsupported"};
    }
    using Definition = details::QueueNodeDefinition<void, decltype(start_callback), decltype(stop_callback), true>;
    return RuntimeSignatureNodeTemplate<Definition>{Definition{
        std::move(start_callback), std::move(stop_callback), mode_value, output}};
}

} // namespace bg
