#include <bg/python/gil.hpp>

#include <exception>

namespace bg::python {

void report_unraisable(std::exception_ptr failure) noexcept {
    if (!Py_IsInitialized()) {
        return;
    }

    try {
        auto gil = nanobind::gil_scoped_acquire{};
        try {
            std::rethrow_exception(std::move(failure));
        } catch (nanobind::python_error &error) {
            error.discard_as_unraisable(Py_None);
        } catch (const std::exception &error) {
            PyErr_SetString(PyExc_RuntimeError, error.what());
            PyErr_WriteUnraisable(Py_None);
        } catch (...) {
            PyErr_SetString(PyExc_RuntimeError, "an unknown exception occurred in a native Python-aware node");
            PyErr_WriteUnraisable(Py_None);
        }
    } catch (...) {
        // Interpreter shutdown can prevent reporting from a noexcept callback.
    }
}

} // namespace bg::python
