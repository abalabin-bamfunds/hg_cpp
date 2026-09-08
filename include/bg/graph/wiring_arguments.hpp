#pragma once

#include <bg/graph/details/wiring_argument.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace bg {

class WiringArguments {
  public:
    std::size_t size() const noexcept { return arguments_.size(); }

    util::symbol parameter_name(std::size_t index) const noexcept { return signature_->parameter(index).name; }

    std::size_t parameter_index(util::symbol name) const { return find_parameter(name.text()); }

    std::size_t parameter_index(std::string_view name) const { return find_parameter(name); }

    bool present(std::size_t index) const noexcept { return arguments_[index].present(); }

    bool present(util::symbol name) const { return present(parameter_index(name)); }

    bool present(std::string_view name) const { return present(parameter_index(name)); }

    const Type &original_type(std::size_t index) const noexcept { return arguments_[index].type(); }

    const Type &original_type(util::symbol name) const { return original_type(parameter_index(name)); }

    const Type &original_type(std::string_view name) const { return original_type(parameter_index(name)); }

    const Type &resolved_type(std::size_t index) const {
        return signature_->parameter(index).type->resolve(*type_map_);
    }

    const Type &resolved_type(util::symbol name) const { return resolved_type(parameter_index(name)); }

    const Type &resolved_type(std::string_view name) const { return resolved_type(parameter_index(name)); }

    const TypeMap &type_map() const noexcept { return *type_map_; }

    std::optional<Scalar<>> scalar(std::size_t index) const {
        const auto &argument = arguments_[index];
        if (!argument.is_scalar() || !argument.present()) {
            return std::nullopt;
        }

        auto value = argument.scalar_view() == nullptr
                         ? Scalar<>::from(argument.scalar_runtime_type(), argument.scalar_data())
                         : Scalar<>::from(*argument.scalar_view());
        if (scalar_read_ != nullptr) {
            scalar_read_(scalar_read_context_, index, value);
        }
        return value;
    }

    std::optional<Scalar<>> scalar(util::symbol name) const { return scalar(parameter_index(name)); }

    std::optional<Scalar<>> scalar(std::string_view name) const { return scalar(parameter_index(name)); }

  private:
    friend class WiringTemplate;
    friend class Operator;

    using ScalarReadFunction = void (*)(void *, std::size_t, Scalar<>);

    WiringArguments(const Signature &signature, const TypeMap &type_map,
                    std::span<const details::wiring_argument> arguments, void *scalar_read_context = nullptr,
                    ScalarReadFunction scalar_read = nullptr) noexcept
        : signature_{&signature}, type_map_{&type_map}, arguments_{arguments},
          scalar_read_context_{scalar_read_context}, scalar_read_{scalar_read} {}

    std::size_t find_parameter(std::string_view name) const {
        for (auto index = std::size_t{}; index < signature_->parameter_count(); ++index) {
            if (signature_->parameter(index).name.text() == name) {
                return index;
            }
        }

        throw std::out_of_range{"unknown wiring parameter"};
    }

    const Signature *signature_;
    const TypeMap *type_map_;
    std::span<const details::wiring_argument> arguments_;
    void *scalar_read_context_;
    ScalarReadFunction scalar_read_;
};

} // namespace bg
