#include "delta.hpp"

#include <bg/python/object.hpp>

#include <stdexcept>

namespace nb = nanobind;

namespace bg::python::details {
namespace {

struct DeltaTypes {
    Object set_delta_type;
    Object removed_type;
    Object remove_sentinel;
};

DeltaTypes &delta_types() {
    static auto value = DeltaTypes{};
    return value;
}

} // namespace

void register_delta_types(nb::handle set_delta_type, nb::handle removed_type, nb::handle remove_sentinel) {
    if (!PyType_Check(set_delta_type.ptr()) || !PyType_Check(removed_type.ptr())) {
        throw std::invalid_argument{"delta registration requires Python types"};
    }
    if (remove_sentinel.is_none()) {
        throw std::invalid_argument{"REMOVE registration requires a Python object"};
    }

    auto &types = delta_types();
    types.set_delta_type = Object{set_delta_type};
    types.removed_type = Object{removed_type};
    types.remove_sentinel = Object{remove_sentinel};
}

bool is_remove(nb::handle value) {
    const auto &sentinel = delta_types().remove_sentinel;
    return sentinel && value.ptr() == sentinel.get().ptr();
}

bool is_set_delta(nb::handle value) {
    const auto &type = delta_types().set_delta_type;
    return type && PyObject_IsInstance(value.ptr(), type.get().ptr()) == 1;
}

bool is_removed(nb::handle value) {
    const auto &type = delta_types().removed_type;
    return type && PyObject_IsInstance(value.ptr(), type.get().ptr()) == 1;
}

nb::handle remove_sentinel() {
    const auto &sentinel = delta_types().remove_sentinel;
    if (!sentinel) {
        throw std::logic_error{"Python delta types have not been registered"};
    }
    return sentinel.get();
}

nb::object make_set_delta(nb::handle added, nb::handle removed) {
    const auto &type = delta_types().set_delta_type;
    if (!type) {
        throw std::logic_error{"Python delta types have not been registered"};
    }
    return type.get()(added, removed);
}

} // namespace bg::python::details
