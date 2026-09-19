#include <bg/graph/details/normalized_wiring_arguments.hpp>

#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/wiring_error.hpp>

#include <algorithm>

namespace bg::details {
namespace {

NormalizedWiringArguments normalize(const Signature &signature, std::span<const WiringArgument> arguments,
                                    allocator *memory) {
    const auto variadic = signature.variadic_parameter();
    if (!variadic) {
        auto normalized = std::vector<WiringArgument>{};
        auto descriptors = std::vector<WiringArgumentDescriptor>{};
        normalized.reserve(arguments.size());
        descriptors.reserve(arguments.size());
        for (const auto &argument : arguments) {
            if (argument.name().has_value() && !wiring_argument_access::name_from_variadic_capture(argument)) {
                throw WiringError{"a non-variadic parameter does not accept named arguments"};
            }
            auto value = wiring_argument_access::borrow(argument);
            wiring_argument_access::clear_name(value);
            normalized.push_back(std::move(value));
            descriptors.emplace_back(argument);
        }
        return {std::move(normalized), std::move(descriptors), nullptr};
    }

    if (arguments.size() < *variadic) {
        throw WiringError{"a variadic call is missing a fixed argument"};
    }

    auto normalized = std::vector<WiringArgument>{};
    auto descriptors = std::vector<WiringArgumentDescriptor>{};
    normalized.reserve(*variadic + 1);
    descriptors.reserve(arguments.size());
    for (const auto &argument : arguments.first(*variadic)) {
        normalized.push_back(wiring_argument_access::borrow(argument));
        descriptors.emplace_back(argument);
    }
    if (std::ranges::any_of(normalized, [](const auto &argument) { return argument.name().has_value(); })) {
        throw WiringError{"a fixed parameter does not accept a named argument"};
    }

    const auto &parameter = signature.parameter(*variadic);
    const auto &operations = *parameter.variadic_operations;
    const auto named = parameter.variadic_mode == VariadicMode::named;
    const auto name_conflicts_with_fixed = [&](util::symbol name) {
        if (!named) {
            return false;
        }
        for (auto index = std::size_t{}; index < *variadic; ++index) {
            if (signature.parameter(index).name == name) {
                return true;
            }
        }
        return false;
    };
    const auto append_record_arguments = [&](const VariadicWiringRecord &record) {
        auto record_arguments = record.argument_descriptors();
        for (const auto &argument : record_arguments) {
            if (argument.name() && name_conflicts_with_fixed(*argument.name())) {
                throw WiringError{"a variadic argument name conflicts with a fixed parameter"};
            }
            descriptors.push_back(argument);
        }
    };

    const auto captured = arguments.subspan(*variadic);
    for (const auto &argument : captured) {
        if (argument.name().has_value() && name_conflicts_with_fixed(*argument.name())) {
            throw WiringError{"a variadic argument name conflicts with a fixed parameter"};
        }
        if (argument.is_time_series()) {
            const auto *record = variadic_wiring_record(argument.time_series());
            if (record != nullptr && captured.size() != 1) {
                throw WiringError{"a forwarded variadic wiring record cannot have additional arguments"};
            }
        }
    }

    if (captured.size() == 1 && captured.front().is_time_series()) {
        if (const auto *record = variadic_wiring_record(captured.front().time_series())) {
            if (record->mode() != parameter.variadic_mode) {
                throw WiringError{"a variadic wiring record has an incompatible capture mode"};
            }
            append_record_arguments(*record);
            normalized.push_back(captured.front());
            return {std::move(normalized), std::move(descriptors), nullptr};
        }
    }

    const auto direct = captured.size() == 1 && captured.front().is_time_series() &&
                        captured.front().name() == std::optional{parameter.name} &&
                        operations.accepts_direct(static_cast<const TimeSeriesType &>(captured.front().type()));
    if (direct) {
        const auto collection = captured.front().time_series();
        if (memory != nullptr) {
            const auto point = add_wiring_record(
                std::make_unique<VariadicWiringRecord>(collection, parameter.variadic_mode, operations));
            const auto &record = *variadic_wiring_record(point);
            append_record_arguments(record);
            normalized.emplace_back(point);
            return {std::move(normalized), std::move(descriptors), nullptr};
        }

        auto record = std::make_unique<VariadicWiringRecord>(collection, parameter.variadic_mode, operations);
        append_record_arguments(*record);
        normalized.emplace_back(record->probe_point());
        return {std::move(normalized), std::move(descriptors), std::move(record)};
    }

    auto remaining = std::vector<WiringArgument>{};
    remaining.reserve(captured.size());
    for (const auto &argument : captured) {
        remaining.push_back(wiring_argument_access::borrow(argument));
    }

    if (memory != nullptr) {
        const auto point = add_wiring_record(std::make_unique<VariadicWiringRecord>(
            static_cast<const TimeSeriesType &>(*parameter.type), parameter.variadic_mode, operations,
            std::move(remaining), memory));
        const auto &record = *variadic_wiring_record(point);
        append_record_arguments(record);
        normalized.emplace_back(point);
        return {std::move(normalized), std::move(descriptors), nullptr};
    }

    auto record = std::make_unique<VariadicWiringRecord>(
        static_cast<const TimeSeriesType &>(*parameter.type), parameter.variadic_mode, operations,
        std::move(remaining), nullptr);
    append_record_arguments(*record);
    normalized.emplace_back(record->probe_point());
    return {std::move(normalized), std::move(descriptors), std::move(record)};
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
