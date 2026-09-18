#include <bg/graph/details/variadic_wiring_record.hpp>

#include <bg/graph/wiring_error.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <algorithm>
#include <string>

namespace bg::details {
namespace {

const TimeSeriesType &target_type_of(const TimeSeriesType &type) noexcept {
    if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&type)) {
        return reference->target_type();
    }
    return type;
}

} // namespace

VariadicWiringRecord::VariadicWiringRecord(CapturedArguments captured, VariadicMode mode)
    : WiringRecord{*captured.type}, arguments_{std::move(captured.arguments)}, mode_{mode} {}

VariadicWiringRecord::CapturedArguments VariadicWiringRecord::capture(VariadicMode mode,
                                                                     std::vector<WiringArgument> arguments,
                                                                     allocator *memory) {
    auto fields = std::vector<BundleField>{};
    auto names = std::vector<util::symbol>{};
    auto captured = std::vector<WiringArgument>{};
    fields.reserve(arguments.size());
    names.reserve(arguments.size());
    captured.reserve(arguments.size());

    for (auto &argument : arguments) {
        if (mode == VariadicMode::positional && argument.name().has_value() &&
            !wiring_argument_access::name_from_variadic_capture(argument)) {
            throw WiringError{"var_args does not accept named arguments"};
        }
        if (mode == VariadicMode::named && !argument.name().has_value()) {
            throw WiringError{"var_kwargs requires named arguments"};
        }
        if (mode == VariadicMode::named) {
            const auto name = *argument.name();
            if (std::ranges::find(names, name) != names.end()) {
                throw WiringError{"a variadic parameter has duplicate argument names"};
            }
            names.push_back(name);
        }
        if (argument.is_scalar() && !argument.present()) {
            continue;
        }

        const auto name =
            mode == VariadicMode::positional ? util::symbol{std::to_string(fields.size())} : *argument.name();
        const auto *type = &argument.type();
        if (argument.is_scalar()) {
            const auto &value = time_series_value_type(static_cast<const ScalarType &>(argument.type()),
                                                       TimeSeriesGuarantee::const_);
            (void)resolve_time_series_runtime_type(value);
            type = &value;
        }
        fields.push_back({name, static_cast<const TimeSeriesType *>(type)});
        wiring_argument_access::set_variadic_name(argument, name);
        if (memory == nullptr) {
            captured.push_back(wiring_argument_access::borrow(argument));
        } else {
            captured.push_back(wiring_argument_access::clone(argument, *memory));
        }
    }

    return {&time_series_bundle_type(unnamed_bundle_schema(fields), TimeSeriesGuarantee::ordinary),
            std::move(captured)};
}

WiringPoint VariadicWiringRecord::materialize(const TimeSeriesType &target_type) const {
    const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(&target_type_of(target_type));
    if (bundle == nullptr) {
        throw WiringError{"a variadic parameter requires a TSB parameter"};
    }

    auto fields = std::vector<NamedBundleWiringPoint>{};
    fields.reserve(arguments_.size());
    for (const auto &argument : arguments_) {
        const auto point = argument.is_time_series() ? argument.time_series()
                                                     : static_cast<WiringPoint>(operators::const_(argument));
        fields.push_back({*argument.name(), point});
    }
    return materialize_wiring_point(materialize_time_series_bundle(fields, *bundle), target_type);
}

const VariadicWiringRecord *variadic_wiring_record(WiringPoint point) noexcept {
    return dynamic_cast<const VariadicWiringRecord *>(&wiring_record(point));
}

WiringPoint make_variadic_wiring_record(VariadicMode mode, std::vector<WiringArgument> arguments,
                                        allocator &memory) {
    auto captured = VariadicWiringRecord::capture(mode, std::move(arguments), &memory);
    return add_wiring_record(std::make_unique<VariadicWiringRecord>(std::move(captured), mode));
}

std::unique_ptr<VariadicWiringRecord>
make_unregistered_variadic_wiring_record(VariadicMode mode, std::vector<WiringArgument> arguments) {
    auto captured = VariadicWiringRecord::capture(mode, std::move(arguments), nullptr);
    return std::make_unique<VariadicWiringRecord>(std::move(captured), mode);
}

} // namespace bg::details
