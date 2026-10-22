#include "python_wiring_error.hpp"

#include <bg/graph/wiring_error.hpp>

#include <nanobind/nanobind.h>

#include <Python.h>

#include <exception>

namespace nb = nanobind;

namespace bg::python::details {
namespace {

std::exception_ptr python_error_cause(std::exception_ptr cause) {
    if (cause == nullptr) {
        return {};
    }

    try {
        std::rethrow_exception(cause);
    } catch (const WiringError &error) {
        return python_error_cause(error.cause());
    } catch (const nb::python_error &) {
        return cause;
    } catch (...) {
        return {};
    }
}

} // namespace

bool has_python_error(const WiringError &error) {
    return python_error_cause(error.cause()) != nullptr;
}

[[noreturn]] void rethrow_python_error(const WiringError &error) {
    const auto cause = python_error_cause(error.cause());
    if (cause == nullptr) {
        throw error;
    }

    try {
        std::rethrow_exception(cause);
    } catch (nb::python_error &python_exception) {
        auto preserved = nb::python_error{python_exception};
        if (!error.contexts().empty()) {
            const auto context = WiringError::from_exception("BG wiring context", error.contexts(), {});
            try {
                const auto add_note = nb::getattr(nb::borrow<nb::object>(PyExc_BaseException), "add_note");
                add_note(preserved.value(), nb::str{context.what()});
            } catch (const nb::python_error &) {
                PyErr_Clear();
            }
        }
        preserved.restore();
        nb::raise_python_error();
    }

    std::terminate();
}

} // namespace bg::python::details
