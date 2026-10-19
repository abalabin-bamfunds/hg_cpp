#pragma once

#include <bg/python/export.hpp>

#include <Python.h>
#include <nanobind/nanobind.h>

#include <exception>
#include <stdexcept>

namespace bg::python {

class GilScope {
  public:
    GilScope() noexcept {
        if (Py_IsInitialized() != 0) {
            state_ = PyGILState_Ensure();
            acquired_ = true;
        }
    }

    ~GilScope() {
        if (acquired_) {
            PyGILState_Release(state_);
        }
    }

    explicit operator bool() const noexcept { return acquired_; }

    void check_error() const {
        if (!acquired_) {
            throw std::runtime_error{"the Python interpreter has shut down"};
        }
        if (PyErr_Occurred() != nullptr) {
            nanobind::raise_python_error();
        }
    }

    void report_error() const noexcept {
        if (acquired_ && PyErr_Occurred() != nullptr) {
            PyErr_WriteUnraisable(Py_None);
        }
    }

  private:
    PyGILState_STATE state_{PyGILState_UNLOCKED};
    bool acquired_{};
};

BG_PYTHON_API void report_unraisable(std::exception_ptr failure) noexcept;

} // namespace bg::python
