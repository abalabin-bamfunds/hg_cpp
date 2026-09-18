#pragma once

#include <bg/graph/wiring_argument.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bg {

class WiringArguments {
  public:
    std::size_t size() const noexcept { return arguments_.size(); }

    util::symbol argument_name(std::size_t index) const {
        if (index < fixed_argument_count()) {
            return signature_->parameter(index).name;
        }
        const auto variadic = *signature_->variadic_parameter();
        if (signature_->parameter(variadic).variadic_mode == details::VariadicMode::positional) {
            return util::symbol{std::to_string(index)};
        }
        return *arguments_[index].name();
    }

    std::size_t argument_index(util::symbol name) const { return find_argument(name.text()); }

    std::size_t argument_index(std::string_view name) const { return find_argument(name); }

    bool present(std::size_t index) const noexcept { return arguments_[index].present(); }

    bool present(util::symbol name) const { return present(argument_index(name)); }

    bool present(std::string_view name) const { return present(argument_index(name)); }

    const Type &original_type(std::size_t index) const noexcept { return arguments_[index].type(); }

    const Type &original_type(util::symbol name) const { return original_type(argument_index(name)); }

    const Type &original_type(std::string_view name) const { return original_type(argument_index(name)); }

    const Type &resolved_type(std::size_t index) const {
        if (index >= fixed_argument_count()) {
            return original_type(index);
        }
        return signature_->parameter(index).type->resolve(*type_map_);
    }

    const Type &resolved_type(util::symbol name) const { return resolved_type(argument_index(name)); }

    const Type &resolved_type(std::string_view name) const { return resolved_type(argument_index(name)); }

    const TypeMap &type_map() const noexcept { return *type_map_; }

    std::optional<Scalar<>> scalar(std::size_t index) const {
        const auto &argument = arguments_[index];
        if (!argument.is_scalar() || !argument.present()) {
            return std::nullopt;
        }

        auto value = argument.scalar();
        if (scalar_read_ != nullptr) {
            scalar_read_(scalar_read_context_, index, value);
        }
        return value;
    }

    std::optional<Scalar<>> scalar(util::symbol name) const { return scalar(argument_index(name)); }

    std::optional<Scalar<>> scalar(std::string_view name) const { return scalar(argument_index(name)); }

  private:
    friend class WiringTemplate;
    friend class Operator;
    friend class Signature;

    using ScalarReadFunction = void (*)(void *, std::size_t, Scalar<>);

    WiringArguments(const Signature &signature, const TypeMap &type_map,
                    std::span<const WiringArgument> arguments, void *scalar_read_context = nullptr,
                    ScalarReadFunction scalar_read = nullptr) noexcept
        : signature_{&signature}, type_map_{&type_map}, arguments_{arguments},
          scalar_read_context_{scalar_read_context}, scalar_read_{scalar_read} {}

    std::size_t fixed_argument_count() const noexcept {
        return signature_->variadic_parameter().value_or(signature_->parameter_count());
    }

    std::size_t find_argument(std::string_view name) const {
        for (auto index = std::size_t{}; index < arguments_.size(); ++index) {
            if (argument_name(index).text() == name) {
                return index;
            }
        }

        throw std::out_of_range{"unknown wiring argument"};
    }

    const Signature *signature_;
    const TypeMap *type_map_;
    std::span<const WiringArgument> arguments_;
    void *scalar_read_context_;
    ScalarReadFunction scalar_read_;
};

} // namespace bg
