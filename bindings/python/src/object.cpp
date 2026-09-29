#include <bg/python/object.hpp>

#include <stdexcept>

namespace bg::python {

std::mutex Object::deferred_releases_mutex_;
std::vector<PyObject *> Object::deferred_releases_;

Object::Object(nanobind::handle value) {
    auto *python_object = value.ptr();
    if (python_object == nullptr) {
        throw std::invalid_argument{"a Python object cannot be null"};
    }

    Py_INCREF(python_object);
    value_ = std::shared_ptr<PyObject>{python_object, &Object::release};
}

nanobind::handle Object::get() const noexcept {
    return nanobind::handle{value_.get()};
}

WeakObject::WeakObject(nanobind::handle value) : reference_{nanobind::weakref{value}} {}

Object WeakObject::lock() const {
    auto value = nanobind::steal<nanobind::object>(PyObject_CallNoArgs(reference_.get().ptr()));
    if (!value) {
        nanobind::raise_python_error();
    }
    if (value.is_none()) {
        throw std::runtime_error{"a Python graph or node callback was released before it was used"};
    }
    return Object{value};
}

void Object::release(PyObject *value) noexcept {
    if (Py_IsInitialized() && PyGILState_Check()) {
        Py_DECREF(value);
        return;
    }

    try {
        const std::lock_guard lock{deferred_releases_mutex_};
        deferred_releases_.push_back(value);
    } catch (...) {
        // Keep the Python reference alive if shutdown-time queue growth fails.
    }
}

void Object::drain_deferred_releases() {
    if (!Py_IsInitialized()) {
        return;
    }
    if (!PyGILState_Check()) {
        throw std::logic_error{"deferred Python references can only be released while holding the GIL"};
    }

    auto pending = std::vector<PyObject *>{};
    {
        const std::lock_guard lock{deferred_releases_mutex_};
        pending.swap(deferred_releases_);
    }

    for (auto *value : pending) {
        Py_DECREF(value);
    }
}

} // namespace bg::python
