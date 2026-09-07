#include <bg/node/details/output_runtime_type.hpp>

#include <bg/scalar/runtime_type.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <cstddef>

namespace bg::details {

const TimeSeriesRuntimeType &
resolve_generic_output_runtime_type(const ResolvedSignature &signature,
                                    std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                                    std::span<const TimeSeriesRuntimeType *const> time_series_input_runtime_types) {
    const TimeSeriesReferenceType *reference{};
    const TimeSeriesValueType *output_value{};
    signature.output->visit([&](const TimeSeriesReferenceType *type) { reference = type; },
                            [&](const TimeSeriesValueType *type) { output_value = type; }, ddv::noop);

    if (reference != nullptr) {
        auto input = std::size_t{};
        for (const auto &parameter : signature.parameters) {
            if (!parameter.type->is_time_series()) {
                continue;
            }

            const auto *runtime_type = time_series_input_runtime_types[input++];
            if (&runtime_type->type() == &reference->target_type()) {
                return time_series_reference_runtime_type(*reference, *runtime_type);
            }

            const TimeSeriesRuntimeType *target{};
            runtime_type->type().visit(
                [&](const TimeSeriesReferenceType *) {
                    target = &static_cast<const TimeSeriesReferenceRuntimeType &>(*runtime_type).target_runtime_type();
                },
                ddv::noop);
            if (target != nullptr && &target->type() == &reference->target_type()) {
                return time_series_reference_runtime_type(*reference, *target);
            }
        }

        const TimeSeriesValueType *value{};
        reference->target_type().visit([&](const TimeSeriesValueType *type) { value = type; }, ddv::noop);
        if (value != nullptr) {
            const auto &scalar = resolve_scalar_runtime_type(value->value_type(), {});
            const auto &target = time_series_value_runtime_type(reference->target_type(), scalar);
            return time_series_reference_runtime_type(*reference, target);
        }

        return resolve_time_series_runtime_type(*signature.output, time_series_input_runtime_types,
                                                parameter_runtime_types);
    }

    if (output_value == nullptr) {
        auto time_series_input = std::size_t{};
        for (const auto &parameter : signature.parameters) {
            if (!parameter.type->is_time_series()) {
                continue;
            }
            if (parameter.type == signature.output) {
                return *time_series_input_runtime_types[time_series_input];
            }
            ++time_series_input;
        }

        return resolve_time_series_runtime_type(*signature.output, time_series_input_runtime_types,
                                                parameter_runtime_types);
    }

    const ScalarRuntimeType *value_runtime_type{};
    for (auto parameter_index = std::size_t{}; parameter_index < signature.parameters.size(); ++parameter_index) {
        const auto &parameter = signature.parameters[parameter_index];
        if (parameter.type->is_time_series()) {
            const TimeSeriesValueType *input_type{};
            parameter.type->visit([&](const TimeSeriesValueType *type) { input_type = type; }, ddv::noop);
            if (input_type != nullptr && &input_type->value_type() == &output_value->value_type()) {
                value_runtime_type = parameter_runtime_types[parameter_index];
            }
            continue;
        }

        const auto *runtime_type = parameter_runtime_types[parameter_index];
        if (runtime_type == nullptr) {
            continue;
        }
        if (const auto *candidate = runtime_type->runtime_type_for(output_value->value_type())) {
            value_runtime_type = candidate;
        }
    }

    if (value_runtime_type != nullptr) {
        return time_series_value_runtime_type(*signature.output, *value_runtime_type);
    }

    return resolve_time_series_runtime_type(*signature.output, time_series_input_runtime_types,
                                            parameter_runtime_types);
}

} // namespace bg::details
