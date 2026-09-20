#include <bg/graph/details/normalized_wiring_arguments.hpp>

#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/wiring_error.hpp>

#include <algorithm>

namespace bg::details {
namespace {

struct VariadicCaptureArguments {
    std::size_t parameter_index;
    std::vector<const WiringArgument *> arguments;
};

NormalizedWiringArguments normalize(const Signature &signature, std::span<const WiringArgument> arguments,
                                    allocator *memory) {
    const auto first_variadic = signature.first_variadic_parameter();
    if (!first_variadic) {
        auto normalized = std::vector<WiringArgument>{};
        auto descriptors = std::vector<WiringArgumentDescriptor>{};
        normalized.reserve(arguments.size());
        descriptors.reserve(arguments.size());
        for (const auto &argument : arguments) {
            if (argument.name().has_value() &&
                wiring_argument_access::variadic_capture_mode(argument) == VariadicMode::none) {
                throw WiringError{"a non-variadic parameter does not accept named arguments"};
            }
            auto value = wiring_argument_access::borrow(argument);
            wiring_argument_access::clear_name(value);
            normalized.push_back(std::move(value));
            descriptors.emplace_back(argument);
        }
        return {std::move(normalized), std::move(descriptors), {}};
    }

    if (arguments.size() < *first_variadic) {
        throw WiringError{"a variadic call is missing a fixed argument"};
    }

    auto normalized = std::vector<WiringArgument>{};
    auto descriptors = std::vector<WiringArgumentDescriptor>{};
    auto probe_records = std::vector<std::unique_ptr<WiringRecord>>{};
    normalized.reserve(signature.parameter_count());
    descriptors.reserve(arguments.size());
    probe_records.reserve(signature.parameter_count() - *first_variadic);

    for (const auto &argument : arguments.first(*first_variadic)) {
        normalized.push_back(wiring_argument_access::borrow(argument));
        descriptors.emplace_back(argument);
    }
    if (std::ranges::any_of(normalized, [](const auto &argument) { return argument.name().has_value(); })) {
        throw WiringError{"a fixed parameter does not accept a named argument"};
    }

    auto positional = std::optional<VariadicCaptureArguments>{};
    auto named = std::optional<VariadicCaptureArguments>{};
    if (const auto index = signature.variadic_parameter(VariadicMode::positional)) {
        positional.emplace(*index);
    }
    if (const auto index = signature.variadic_parameter(VariadicMode::named)) {
        named.emplace(*index);
    }

    const auto capture_for = [&](VariadicMode mode) -> VariadicCaptureArguments & {
        if (mode == VariadicMode::positional && positional) {
            return *positional;
        }
        if (mode == VariadicMode::named && named) {
            return *named;
        }
        if (positional) {
            return *positional;
        }
        return *named;
    };

    auto named_arguments_started = false;
    for (const auto &argument : arguments.subspan(*first_variadic)) {
        auto mode = wiring_argument_access::variadic_capture_mode(argument);
        const auto *forwarded = argument.is_time_series() ? variadic_wiring_record(argument.time_series()) : nullptr;
        if (forwarded != nullptr) {
            if (argument.name()) {
                throw WiringError{"a forwarded variadic wiring record cannot be named"};
            }
            mode = forwarded->mode();
        } else if (mode == VariadicMode::none) {
            if (argument.name() && positional &&
                *argument.name() == signature.parameter(positional->parameter_index).name) {
                mode = VariadicMode::positional;
            } else {
                mode = argument.name() ? VariadicMode::named : VariadicMode::positional;
            }
        }

        auto &capture = capture_for(mode);
        const auto capture_mode = signature.parameter(capture.parameter_index).variadic_mode;
        if (capture_mode == VariadicMode::positional && named_arguments_started) {
            throw WiringError{"a positional argument cannot follow a named argument"};
        }
        named_arguments_started |= capture_mode == VariadicMode::named;
        capture.arguments.push_back(&argument);
    }

    const auto name_conflicts_with_fixed = [&](util::symbol name) {
        for (auto index = std::size_t{}; index < *first_variadic; ++index) {
            if (signature.parameter(index).name == name) {
                return true;
            }
        }
        return false;
    };

    const auto append_record_descriptors = [&](const VariadicWiringRecord &record) {
        auto record_arguments = record.argument_descriptors();
        for (const auto &argument : record_arguments) {
            if (record.mode() == VariadicMode::named && argument.name() &&
                name_conflicts_with_fixed(*argument.name())) {
                throw WiringError{"a variadic argument name conflicts with a fixed parameter"};
            }
            descriptors.push_back(argument);
        }
    };

    const auto append_capture = [&](VariadicCaptureArguments &capture) {
        const auto &parameter = signature.parameter(capture.parameter_index);
        const auto mode = parameter.variadic_mode;
        const auto &operations = *parameter.variadic_operations;

        if (std::ranges::any_of(capture.arguments, [](const auto *argument) { return argument->is_graph(); })) {
            throw WiringError{"a variadic parameter cannot capture a graph argument"};
        }

        for (const auto *argument : capture.arguments) {
            if (!argument->is_time_series()) {
                continue;
            }
            if (variadic_wiring_record(argument->time_series()) != nullptr && capture.arguments.size() != 1) {
                throw WiringError{"a forwarded variadic wiring record cannot have additional arguments in its pack"};
            }
        }

        if (capture.arguments.size() == 1 && capture.arguments.front()->is_time_series()) {
            const auto &argument = *capture.arguments.front();
            if (const auto *record = variadic_wiring_record(argument.time_series())) {
                if (record->mode() != mode) {
                    throw WiringError{"a variadic wiring record has an incompatible capture mode"};
                }
                append_record_descriptors(*record);
                normalized.push_back(wiring_argument_access::borrow(argument));
                return;
            }

            const auto direct = argument.name() == std::optional{parameter.name} &&
                                operations.accepts_direct(static_cast<const TimeSeriesType &>(argument.type()));
            if (direct) {
                const auto collection = argument.time_series();
                if (memory != nullptr) {
                    const auto point = add_wiring_record(
                        std::make_unique<VariadicWiringRecord>(collection, mode, operations));
                    const auto &record = *variadic_wiring_record(point);
                    append_record_descriptors(record);
                    normalized.emplace_back(point);
                    return;
                }

                auto record = std::make_unique<VariadicWiringRecord>(collection, mode, operations);
                append_record_descriptors(*record);
                normalized.emplace_back(record->probe_point());
                probe_records.push_back(std::move(record));
                return;
            }
        }

        auto captured_names = std::vector<util::symbol>{};
        captured_names.reserve(capture.arguments.size());
        for (const auto *argument : capture.arguments) {
            const auto name = argument->name();
            if (mode == VariadicMode::positional) {
                if (name && wiring_argument_access::variadic_capture_mode(*argument) == VariadicMode::none) {
                    throw WiringError{"var_args does not accept named arguments"};
                }
                continue;
            }
            if (!name) {
                throw WiringError{"var_kwargs requires named arguments"};
            }
            if (name_conflicts_with_fixed(*name)) {
                throw WiringError{"a variadic argument name conflicts with a fixed parameter"};
            }
            if (std::ranges::find(captured_names, *name) != captured_names.end()) {
                throw WiringError{"a variadic parameter has duplicate argument names"};
            }
            captured_names.push_back(*name);
        }

        auto captured = std::vector<WiringArgument>{};
        captured.reserve(capture.arguments.size());
        for (const auto *argument : capture.arguments) {
            captured.push_back(wiring_argument_access::borrow(*argument));
        }

        if (memory != nullptr) {
            const auto point = add_wiring_record(std::make_unique<VariadicWiringRecord>(
                static_cast<const TimeSeriesType &>(*parameter.type), mode, operations, std::move(captured), memory));
            const auto &record = *variadic_wiring_record(point);
            append_record_descriptors(record);
            normalized.emplace_back(point);
            return;
        }

        auto record = std::make_unique<VariadicWiringRecord>(
            static_cast<const TimeSeriesType &>(*parameter.type), mode, operations, std::move(captured), nullptr);
        append_record_descriptors(*record);
        normalized.emplace_back(record->probe_point());
        probe_records.push_back(std::move(record));
    };

    if (positional) {
        append_capture(*positional);
    }
    if (named) {
        append_capture(*named);
    }

    return {std::move(normalized), std::move(descriptors), std::move(probe_records)};
}

} // namespace

NormalizedWiringArguments normalize_wiring_arguments(const Signature &signature,
                                                      std::span<const WiringArgument> arguments,
                                                      allocator &memory) {
    return normalize(signature, arguments, &memory);
}

NormalizedWiringArguments probe_wiring_arguments(const Signature &signature,
                                                  std::span<const WiringArgument> arguments) {
    return normalize(signature, arguments, nullptr);
}

} // namespace bg::details
