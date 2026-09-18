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
        auto logical = std::vector<WiringArgument>{};
        normalized.reserve(arguments.size());
        logical.reserve(arguments.size());
        for (const auto &argument : arguments) {
            if (argument.name().has_value() && !wiring_argument_access::name_from_variadic_capture(argument)) {
                throw WiringError{"a non-variadic parameter does not accept named arguments"};
            }
            auto value = wiring_argument_access::borrow(argument);
            wiring_argument_access::clear_name(value);
            normalized.push_back(std::move(value));
            logical.push_back(wiring_argument_access::borrow(argument));
        }
        return {std::move(normalized), std::move(logical), nullptr};
    }

    if (arguments.size() < *variadic) {
        throw WiringError{"a variadic call is missing a fixed argument"};
    }

    auto normalized = std::vector<WiringArgument>{};
    auto logical = std::vector<WiringArgument>{};
    normalized.reserve(*variadic + 1);
    logical.reserve(arguments.size());
    for (const auto &argument : arguments.first(*variadic)) {
        normalized.push_back(wiring_argument_access::borrow(argument));
        logical.push_back(wiring_argument_access::borrow(argument));
    }
    if (std::ranges::any_of(normalized, [](const auto &argument) { return argument.name().has_value(); })) {
        throw WiringError{"a fixed parameter does not accept a named argument"};
    }

    const auto named = signature.parameter(*variadic).variadic_mode == VariadicMode::named;
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
    const auto append_captured = [&](std::span<const WiringArgument> captured_arguments) {
        for (const auto &argument : captured_arguments) {
            if (name_conflicts_with_fixed(*argument.name())) {
                throw WiringError{"a variadic argument name conflicts with a fixed parameter"};
            }
            logical.push_back(wiring_argument_access::borrow(argument));
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
            if (record->mode() != signature.parameter(*variadic).variadic_mode) {
                throw WiringError{"a variadic wiring record has an incompatible capture mode"};
            }
            append_captured(record->arguments());
            normalized.push_back(captured.front());
            return {std::move(normalized), std::move(logical), nullptr};
        }
    }

    auto remaining = std::vector<WiringArgument>{};
    remaining.reserve(captured.size());
    for (const auto &argument : captured) {
        remaining.push_back(wiring_argument_access::borrow(argument));
    }
    const auto mode = signature.parameter(*variadic).variadic_mode;
    if (memory != nullptr) {
        const auto point = make_variadic_wiring_record(mode, std::move(remaining), *memory);
        const auto &record = *variadic_wiring_record(point);
        append_captured(record.arguments());
        normalized.emplace_back(point);
        return {std::move(normalized), std::move(logical), nullptr};
    }

    auto record = make_unregistered_variadic_wiring_record(mode, std::move(remaining));
    append_captured(record->arguments());
    normalized.emplace_back(record->probe_point());
    return {std::move(normalized), std::move(logical), std::move(record)};
}

} // namespace

NormalizedWiringArguments normalize_wiring_arguments(const Signature &signature,
                                                     std::span<const WiringArgument> arguments, allocator &memory) {
    return normalize(signature, arguments, &memory);
}

NormalizedWiringArguments probe_wiring_arguments(const Signature &signature,
                                                 std::span<const WiringArgument> arguments) {
    return normalize(signature, arguments, nullptr);
}

} // namespace bg::details
