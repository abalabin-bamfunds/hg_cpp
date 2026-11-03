#pragma once

#include <hg/operator.hpp>
#include <hg/graph/wiring_argument.hpp>
#include <hg/python/object.hpp>

#include <nanobind/nanobind.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hg::python {
class PythonWiringPoint;
}

namespace hg::python::details {

class PythonOperator {
  public:
    explicit PythonOperator(const Operator &operation);
    PythonOperator(const Operator &operation, std::span<const Type *const> type_parameters);
    PythonOperator(std::shared_ptr<Operator> operation, std::span<const Type *const> type_parameters);
    PythonOperator(const PythonOperator &other);
    PythonOperator &operator=(const PythonOperator &other);

    nanobind::object call(nanobind::args positional, nanobind::kwargs named) const;
    nanobind::object call_with_receiver(nanobind::handle receiver, nanobind::args positional,
                                        nanobind::kwargs named) const;
    nanobind::object call(nanobind::handle value) const;
    nanobind::object call(nanobind::handle lhs, nanobind::handle rhs) const;
    nanobind::object invoke(std::span<const WiringArgument> arguments) const;
    PythonOperator map_type_arguments(nanobind::handle arguments) const;
    void set_owner(nanobind::handle owner);
    void register_overload(nanobind::handle owner);
    std::string name() const;

  private:
    nanobind::object call(std::span<const nanobind::handle> leading_arguments, nanobind::args positional,
                          nanobind::kwargs named) const;

    const Operator *operation_;
    std::shared_ptr<Operator> owned_operation_;
    std::vector<const Type *> type_parameters_;
    TypeMap type_map_;
    std::optional<WeakObject> owner_;
};

const Type &python_type_argument(nanobind::handle value);

void bind_operators(nanobind::module_ &module);
void register_python_object_operators();
const Operator *find_wiring_method(std::string_view name, const Type &receiver_type);
nanobind::object getitem(const PythonWiringPoint &collection, nanobind::handle key);

} // namespace hg::python::details
