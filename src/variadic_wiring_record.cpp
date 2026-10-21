#include <bg/graph/details/variadic_wiring_record.hpp>

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_error.hpp>

#include <stdexcept>

namespace bg::details {

VariadicWiringRecord::VariadicWiringRecord(VariadicCapture captured, VariadicMode mode,
                                           const VariadicWiringOperations &operations)
    : WiringRecord{*captured.type}, operations_{&operations}, arguments_{std::move(captured.arguments)},
      mode_{mode} {}

VariadicWiringRecord::VariadicWiringRecord(const TimeSeriesType &parameter_type, VariadicMode mode,
                                           const VariadicWiringOperations &operations,
                                           std::vector<WiringArgument> arguments, allocator *memory)
    : VariadicWiringRecord{operations.capture(parameter_type, mode, std::move(arguments), memory), mode,
                           operations} {}

VariadicWiringRecord::VariadicWiringRecord(WiringPoint collection, VariadicMode mode,
                                           const VariadicWiringOperations &operations, allocator *memory)
    : WiringRecord{collection.type()}, operations_{&operations}, mode_{mode} {
    const auto original_arguments = operations.original_arguments(collection);
    if (!original_arguments) {
        direct_ = collection;
        return;
    }

    arguments_.reserve(original_arguments->size());
    for (const auto &original : *original_arguments) {
        auto argument = WiringArgument::borrow(original);
        const auto name = argument.name();
        if (!name) {
            throw std::invalid_argument{"an original variadic collection argument requires a name"};
        }
        argument.set_variadic_name(*name, mode_);
        arguments_.push_back(memory == nullptr ? std::move(argument) : argument.clone(*memory));
    }
}

std::size_t VariadicWiringRecord::size() const noexcept {
    return direct_ ? operations_->item_count(direct_->type()) : arguments_.size();
}

util::symbol VariadicWiringRecord::item_name(std::size_t index) const {
    if (index >= size()) {
        throw std::out_of_range{"a variadic wiring parameter index is out of range"};
    }
    return direct_ ? operations_->item_name(direct_->type(), index) : *arguments_[index].name();
}

WiringArgument VariadicWiringRecord::argument(std::size_t index) const {
    if (index >= size()) {
        throw std::out_of_range{"a variadic wiring parameter index is out of range"};
    }

    if (!direct_) {
        return WiringArgument::borrow(arguments_[index]);
    }

    auto result = WiringArgument{operations_->item(*direct_, index)};
    result.set_variadic_name(operations_->item_name(direct_->type(), index), mode_);
    return result;
}

std::vector<WiringArgumentDescriptor> VariadicWiringRecord::argument_descriptors() const {
    auto result = std::vector<WiringArgumentDescriptor>{};
    result.reserve(size());

    if (!direct_) {
        for (const auto &argument : arguments_) {
            result.emplace_back(argument, mode_);
        }
        return result;
    }

    for (auto index = std::size_t{}; index < size(); ++index) {
        result.emplace_back(operations_->item_name(direct_->type(), index),
                            operations_->item_type(direct_->type(), index), mode_);
    }
    return result;
}

MaterialisationPreparation
VariadicWiringRecord::prepare_materialisation(const TimeSeriesType &target_type) const {
    auto preparation = MaterialisationPreparation{};
    if (direct_) {
        preparation.dependencies.push_back({*direct_, &target_type});
        return preparation;
    }

    preparation.dependencies = operations_->materialisation_dependencies(arguments_, target_type);
    return preparation;
}

WiringPoint VariadicWiringRecord::materialize(const TimeSeriesType &target_type) const {
    if (direct_) {
        return wiring().materialize(*direct_, target_type);
    }
    return operations_->materialize(arguments_, target_type);
}

const VariadicWiringRecord *variadic_wiring_record(WiringPoint point) noexcept {
    return dynamic_cast<const VariadicWiringRecord *>(point.record());
}

} // namespace bg::details
