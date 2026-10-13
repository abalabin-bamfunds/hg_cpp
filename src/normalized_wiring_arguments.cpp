#include <bg/graph/details/normalized_wiring_arguments.hpp>

#include <bg/graph/context.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/details/wiring_record_overlay.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <algorithm>

namespace bg::details {

void append_wiring_arguments(std::vector<WiringArgument> &result, WiringArgumentExpansion expansion) {
    const auto *type = type_cast<TimeSeriesBundleType>(&expansion.bundle.type().target_type());
    if (type == nullptr) {
        throw WiringError{"wiring argument expansion requires a TSB"};
    }

    result.reserve(result.size() + type->item_count());
    for (auto index = std::size_t{}; index < type->item_count(); ++index) {
        const auto field = time_series_bundle_field(expansion.bundle, index);
        if (expansion.mode == VariadicMode::named) {
            result.emplace_back(type->item_name(index), field);
            result.back().match_fixed_parameter_by_name();
        } else {
            result.emplace_back(field);
        }
        if (expansion.requires_optional) {
            result.back().require_optional();
        }
    }
}

namespace {

struct VariadicCaptureArguments {
    std::size_t parameter_index;
    std::vector<const WiringArgument *> arguments;
};

std::vector<WiringArgument> prepare_arguments(const Signature &signature, std::span<const WiringArgument> arguments,
                                              bool import_contexts, const Signature *parameter_aliases) {
    const auto fixed_count = signature.first_variadic_parameter().value_or(signature.parameter_count());
    auto fixed = std::vector<const WiringArgument *>(fixed_count);
    auto variadic = std::vector<const WiringArgument *>{};
    variadic.reserve(arguments.size());

    auto next_positional = std::size_t{};
    for (const auto &argument : arguments) {
        const auto capture_mode = argument.variadic_capture_mode();
        if (capture_mode != VariadicMode::none && fixed_count != signature.parameter_count()) {
            variadic.push_back(&argument);
            continue;
        }

        if (const auto name = argument.name(); name && capture_mode == VariadicMode::none) {
            const auto fixed_parameter = [&]() -> std::optional<std::size_t> {
                for (auto index = std::size_t{}; index < fixed_count; ++index) {
                    const auto &parameter = signature.parameter(index);
                    if (parameter.name == *name &&
                        (parameter.context || parameter_aliases != nullptr ||
                         argument.matches_fixed_parameter_by_name())) {
                        return index;
                    }
                }

                if (parameter_aliases != nullptr) {
                    const auto alias_count = std::min(fixed_count, parameter_aliases->parameter_count());
                    for (auto index = std::size_t{}; index < alias_count; ++index) {
                        if (parameter_aliases->parameter(index).name == *name) {
                            return index;
                        }
                    }
                }
                return std::nullopt;
            }();
            if (fixed_parameter) {
                if (fixed[*fixed_parameter] != nullptr) {
                    throw WiringError{"fixed parameter '" + std::string{name->text()} +
                                      "' received more than one argument"};
                }
                fixed[*fixed_parameter] = &argument;
                continue;
            }

            if (fixed_count == signature.parameter_count()) {
                throw WiringError{"a call has an unknown named argument"};
            }
            variadic.push_back(&argument);
            continue;
        }

        while (next_positional < fixed_count &&
               (fixed[next_positional] != nullptr || signature.parameter(next_positional).context)) {
            ++next_positional;
        }
        if (next_positional < fixed_count) {
            fixed[next_positional++] = &argument;
        } else {
            variadic.push_back(&argument);
        }
    }

    auto prepared = std::vector<WiringArgument>{};
    prepared.reserve(fixed_count + variadic.size());
    for (auto index = std::size_t{}; index < fixed_count; ++index) {
        const auto &parameter = signature.parameter(index);
        const auto *argument = fixed[index];
        auto context_argument = std::optional<WiringArgument>{};
        if (argument == nullptr && parameter.context) {
            context_argument =
                import_contexts ? ContextScope::import(parameter.name) : ContextScope::find(parameter.name);
            if (!context_argument) {
                throw WiringError{"required context '" + std::string{parameter.name.text()} + "' is not available"};
            }
            argument = &*context_argument;
        }
        if (argument == nullptr) {
            throw WiringError{"a call is missing a fixed argument"};
        }

        auto value = WiringArgument::borrow(*argument);
        value.clear_name();
        prepared.push_back(std::move(value));
    }

    for (const auto *argument : variadic) {
        prepared.push_back(WiringArgument::borrow(*argument));
    }
    return prepared;
}

NormalizedWiringArguments normalize_prepared(const Signature &signature, std::vector<WiringArgument> prepared_arguments,
                                             allocator *memory, WiringRecordOverlay *temporary_records) {
    const auto arguments = std::span<const WiringArgument>{prepared_arguments};
    const auto first_variadic = signature.first_variadic_parameter();
    if (!first_variadic) {
        auto normalized = std::vector<WiringArgument>{};
        auto descriptors = std::vector<WiringArgumentDescriptor>{};
        normalized.reserve(arguments.size());
        descriptors.reserve(arguments.size());
        for (const auto &argument : arguments) {
            if (argument.name().has_value() &&
                argument.variadic_capture_mode() == VariadicMode::none) {
                throw WiringError{"a non-variadic parameter does not accept named arguments"};
            }
            auto value = WiringArgument::borrow(argument);
            value.clear_name();
            normalized.push_back(std::move(value));
            descriptors.emplace_back(normalized.back());
        }
        return {std::move(normalized), std::move(descriptors)};
    }

    if (arguments.size() < *first_variadic) {
        throw WiringError{"a variadic call is missing a fixed argument"};
    }

    auto normalized = std::vector<WiringArgument>{};
    auto descriptors = std::vector<WiringArgumentDescriptor>{};
    normalized.reserve(signature.parameter_count());
    descriptors.reserve(arguments.size());

    const auto add_record = [&](std::unique_ptr<WiringRecord> record) {
        return temporary_records != nullptr ? temporary_records->add(std::move(record))
                                            : add_wiring_record(std::move(record));
    };

    for (const auto &argument : arguments.first(*first_variadic)) {
        normalized.push_back(WiringArgument::borrow(argument));
        descriptors.emplace_back(normalized.back());
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
        auto mode = argument.variadic_capture_mode();
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
        auto contains_optional_argument = false;
        for (const auto &argument : record_arguments) {
            if (record.mode() == VariadicMode::named && argument.name() &&
                name_conflicts_with_fixed(*argument.name())) {
                throw WiringError{"a variadic argument name conflicts with a fixed parameter"};
            }
            contains_optional_argument |= argument.requires_optional();
            descriptors.push_back(argument);
        }
        return contains_optional_argument;
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
                const auto contains_optional_argument = append_record_descriptors(*record);
                normalized.push_back(WiringArgument::borrow(argument));
                if (argument.requires_optional() || contains_optional_argument) {
                    normalized.back().require_optional();
                }
                return;
            }

            const auto direct = argument.name() == std::optional{parameter.name} &&
                                operations.accepts_direct(static_cast<const TimeSeriesType &>(argument.type()));
            if (direct) {
                const auto collection = argument.time_series();
                auto record = std::make_unique<VariadicWiringRecord>(collection, mode, operations);
                append_record_descriptors(*record);
                normalized.emplace_back(add_record(std::move(record)));
                if (argument.requires_optional()) {
                    normalized.back().require_optional();
                }
                return;
            }
        }

        auto captured_names = std::vector<util::symbol>{};
        captured_names.reserve(capture.arguments.size());
        for (const auto *argument : capture.arguments) {
            const auto name = argument->name();
            if (mode == VariadicMode::positional) {
                if (name && argument->variadic_capture_mode() == VariadicMode::none) {
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
        auto requires_optional = false;
        for (const auto *argument : capture.arguments) {
            captured.push_back(WiringArgument::borrow(*argument));
            requires_optional |= argument->requires_optional();
        }

        auto record = std::make_unique<VariadicWiringRecord>(static_cast<const TimeSeriesType &>(*parameter.type), mode,
                                                             operations, std::move(captured), memory);
        append_record_descriptors(*record);
        normalized.emplace_back(add_record(std::move(record)));
        if (requires_optional) {
            normalized.back().require_optional();
        }
    };

    if (positional) {
        append_capture(*positional);
    }
    if (named) {
        append_capture(*named);
    }

    return {std::move(normalized), std::move(descriptors)};
}

NormalizedWiringArguments normalize(const Signature &signature, std::span<const WiringArgument> arguments,
                                    bool import_contexts, allocator *memory, WiringRecordOverlay *temporary_records,
                                    const Signature *parameter_aliases) {
    return normalize_prepared(signature, prepare_arguments(signature, arguments, import_contexts, parameter_aliases),
                              memory, temporary_records);
}

} // namespace

NormalizedWiringArguments normalize_wiring_arguments(const Signature &signature,
                                                     std::span<const WiringArgument> arguments, allocator &memory,
                                                     const Signature *parameter_aliases) {
    return normalize(signature, arguments, true, &memory, nullptr, parameter_aliases);
}

NormalizedWiringArguments probe_wiring_arguments(const Signature &signature, std::span<const WiringArgument> arguments,
                                                 WiringRecordOverlay &records,
                                                 const Signature *parameter_aliases) {
    return normalize(signature, arguments, false, nullptr, &records, parameter_aliases);
}

} // namespace bg::details
