#pragma once

#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/guarantee_variable_instance.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/type_variable.hpp>

namespace bg {

class GenericTimeSeriesVariableType final : public TimeSeriesType {
  public:
    GenericTimeSeriesVariableType(const TypeVariableType<TimeSeriesType> &variable, GuaranteeParameter guarantee)
        : TimeSeriesType(guarantee), variable_(&variable) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
    const TypeVariableType<TimeSeriesType> &variable() const noexcept { return *variable_; }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const TypeVariableType<TimeSeriesType> *variable_;
};

namespace details {

template <typename Declaration> struct TimeSeriesVariableInstance;

template <util::compile_time_string Name> struct TimeSeriesVariableInstance<TimeSeriesVariable<Name>> {
    static const TypeVariableType<TimeSeriesType> &get() {
        static const auto variable = TypeVariableType<TimeSeriesType>{util::symbol{Name.view()}};

        return variable;
    }
};

template <typename Variable, auto Guarantee>
struct TimeSeriesVariableInstance<TimeSeriesVariableWithGuarantee<Variable, Guarantee>> {
    static const GenericTimeSeriesVariableType &get() {
        static const auto type = GenericTimeSeriesVariableType{TimeSeriesVariableInstance<Variable>::get(),
                                                               guarantee_parameter<Guarantee>()};

        return type;
    }
};

template <time_series_variable Declaration> const TimeSeriesType &time_series_variable_instance() {
    return TimeSeriesVariableInstance<Declaration>::get();
}

template <util::compile_time_string Name> struct declaration_variable_traits<TimeSeriesVariable<Name>> {
    using Declaration = TimeSeriesVariable<Name>;

    template <typename Visitor> static void for_each(Visitor &&visitor) {
        std::forward<Visitor>(visitor).template operator()<Declaration>(TimeSeriesVariableInstance<Declaration>::get());
    }
};

template <typename Variable, auto Guarantee>
struct declaration_variable_traits<TimeSeriesVariableWithGuarantee<Variable, Guarantee>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<Variable>(visitor);
        if constexpr (guarantee_variable_declaration<decltype(Guarantee)>) {
            using Declaration = std::remove_cv_t<decltype(Guarantee)>;
            visitor.template operator()<Declaration>(guarantee_variable_instance<Guarantee>());
        }
    }
};

} // namespace details

} // namespace bg
