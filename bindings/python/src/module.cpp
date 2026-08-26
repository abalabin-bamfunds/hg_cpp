#include <bg/bg.hpp>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

PYBIND11_MODULE(_bg, module) {
    module.doc() = "Python bindings for bg";
    module.def("version", &bg::version);
}
