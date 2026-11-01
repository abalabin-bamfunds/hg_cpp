#pragma once

#include <bg/python/export.hpp>

#include <nanobind/nanobind.h>

#include <Python.h>

#include <memory>
#include <mutex>
#include <vector>

namespace bg::python {

// Keeps a Python reference valid when native graph storage copies or destroys it without the GIL.
class BG_PYTHON_API Object {
  public:
    Object() noexcept = default;
    explicit Object(nanobind::handle value);
    explicit operator bool() const noexcept { return value_ != nullptr; }
    nanobind::handle get() const noexcept;

    // Drain deferred releases at Python execution boundaries while the GIL is held.
    static void drain_deferred_releases();

    // Releases a new reference immediately when the GIL is held or defers it otherwise.
    static void release_reference(PyObject *value) noexcept;

  private:
    static void release(PyObject *value) noexcept;

    static std::mutex deferred_releases_mutex_;
    static std::vector<PyObject *> deferred_releases_;

    std::shared_ptr<PyObject> value_;
};

// Retains a Python weak reference without requiring the weak reference target to outlive this object.
class BG_PYTHON_API WeakObject {
  public:
    WeakObject() noexcept = default;
    explicit WeakObject(nanobind::handle value);

    Object lock() const;

  private:
    Object reference_;
};

} // namespace bg::python
