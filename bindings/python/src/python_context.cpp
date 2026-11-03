#include "python_context.hpp"

#include <hg/graph/context.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/python/wiring_point.hpp>

#include <Python.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace hg::python::details {
namespace {

struct LocalValue {
    std::string name;
    nb::object value;
};

struct PythonContext {
    nb::object point_object;
    nb::object frame;
    WiringArgument argument;
    std::vector<LocalValue> locals_at_enter;
    std::unique_ptr<hg::details::ContextScope> scope;

    PythonContext(nb::object point, nb::object active_frame, WiringArgument binding,
                  std::vector<LocalValue> locals)
        : point_object{std::move(point)}, frame{std::move(active_frame)}, argument{std::move(binding)},
          locals_at_enter{std::move(locals)} {}

    util::symbol context_name() const;
    void activate();
};

std::vector<PythonContext> &active_contexts() {
    static thread_local auto contexts = std::vector<PythonContext>{};
    return contexts;
}

std::vector<LocalValue> frame_locals(PyObject *frame) {
    auto locals = nb::steal<nb::object>(PyObject_GetAttrString(frame, "f_locals"));
    if (!locals) {
        nb::raise_python_error();
    }

    auto items = nb::steal<nb::object>(PyMapping_Items(locals.ptr()));
    if (!items) {
        nb::raise_python_error();
    }

    auto result = std::vector<LocalValue>{};
    const auto count = PySequence_Size(items.ptr());
    if (count < 0) {
        nb::raise_python_error();
    }
    result.reserve(static_cast<std::size_t>(count));
    for (auto index = Py_ssize_t{}; index < count; ++index) {
        const auto item = PySequence_GetItem(items.ptr(), index);
        if (item == nullptr) {
            nb::raise_python_error();
        }
        auto entry = nb::steal<nb::object>(item);
        const auto key = PyTuple_GetItem(entry.ptr(), 0);
        const auto value = PyTuple_GetItem(entry.ptr(), 1);
        if (key == nullptr || value == nullptr || !PyUnicode_Check(key)) {
            continue;
        }

        auto name_size = Py_ssize_t{};
        const auto *name = PyUnicode_AsUTF8AndSize(key, &name_size);
        if (name == nullptr) {
            nb::raise_python_error();
        }
        result.push_back({std::string{name, static_cast<std::size_t>(name_size)}, nb::borrow<nb::object>(value)});
    }
    return result;
}

util::symbol PythonContext::context_name() const {
    const auto locals = frame_locals(frame.ptr());
    auto changed_names = std::vector<std::string_view>{};
    auto matching_names = std::vector<std::string_view>{};
    for (const auto &local : locals) {
        if (local.value.ptr() != point_object.ptr()) {
            continue;
        }
        matching_names.push_back(local.name);
        const auto old_value = std::ranges::find(locals_at_enter, local.name, &LocalValue::name);
        if (old_value == locals_at_enter.end() || old_value->value.ptr() != local.value.ptr()) {
            changed_names.push_back(local.name);
        }
    }

    auto candidates = changed_names;
    if (candidates.empty()) {
        candidates = matching_names;
    }
    if (candidates.size() != 1) {
        throw std::invalid_argument{
            candidates.empty() ? "a Python context input must be assigned to a local name with 'as name'"
                               : "a Python context input has more than one matching local name"};
    }
    return util::symbol{candidates.front()};
}

void PythonContext::activate() {
    if (scope != nullptr) {
        return;
    }

    scope = std::make_unique<hg::details::ContextScope>(context_name(), std::move(argument));
}

} // namespace

void enter_context(nb::handle point_object) {
    activate_contexts();
    auto &contexts = active_contexts();
    auto *frame = PyEval_GetFrame();
    if (frame == nullptr) {
        throw std::logic_error{"a Python context input requires an active Python frame"};
    }

    const auto &point = nb::cast<const PythonWiringPoint &>(point_object);
    auto &wiring = WiringContext::current().wiring();
    auto argument = point.argument(wiring);
    auto *frame_object = reinterpret_cast<PyObject *>(frame);
    Py_INCREF(frame_object);
    auto frame_reference = nb::steal<nb::object>(frame_object);
    auto locals = frame_locals(frame_reference.ptr());
    auto context = PythonContext{nb::borrow<nb::object>(point_object), std::move(frame_reference),
                                 std::move(argument), std::move(locals)};
    contexts.push_back(std::move(context));
}

void exit_context(nb::handle point_object) {
    auto &contexts = active_contexts();
    if (contexts.empty() || contexts.back().point_object.ptr() != point_object.ptr()) {
        throw std::logic_error{"Python context inputs must exit in reverse entry order"};
    }
    contexts.pop_back();
}

void activate_contexts() {
    for (auto &context : active_contexts()) {
        context.activate();
    }
}

} // namespace hg::python::details
