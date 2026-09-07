#pragma once

#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/guarantee_variable_instance.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/type_variable.hpp>

namespace bg {

class GenericTimeSeriesVariableType final : public TimeSeriesType {
  public:
    GenericTimeSeriesVariableType(const TypeVariable<TimeSeriesType> &variable, GuaranteeParameter guarantee)
        : TimeSeriesType(guarantee), variable_(&variable) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override;

  private:
    const TypeVariable<TimeSeriesType> *variable_;
};

namespace details {

template <typename Scope, typename Declaration> struct TimeSeriesVariableInstance;

template <typename Scope, util::compile_time_string Name>
struct TimeSeriesVariableInstance<Scope, TimeSeriesVariable<Name>> {
    static const TypeVariable<TimeSeriesType> &get() {
        static const auto variable = TypeVariable<TimeSeriesType>{util::symbol{Name.view()}};

        return variable;
    }
};

template <typename Scope, typename Variable, auto Guarantee>
struct TimeSeriesVariableInstance<Scope, TimeSeriesVariableWithGuarantee<Variable, Guarantee>> {
    static const GenericTimeSeriesVariableType &get() {
        static const auto type = GenericTimeSeriesVariableType{TimeSeriesVariableInstance<Scope, Variable>::get(),
                                                               guarantee_parameter<Scope, Guarantee>()};

        return type;
    }
};

template <typename Scope, time_series_variable Declaration>
const TimeSeriesType &time_series_variable_instance() {
    return TimeSeriesVariableInstance<Scope, Declaration>::get();
}

} // namespace details

} // namespace bg
