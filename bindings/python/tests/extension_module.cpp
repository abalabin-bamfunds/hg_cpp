#include <bg/python/scalar_conversion.hpp>

#include <nanobind/nanobind.h>

#include <cstdint>

NB_MODULE(_bg_test_extension, module) {
    module.def("scalar_conversions_address", [] {
        return reinterpret_cast<std::uintptr_t>(&bg::python::scalar_conversions());
    });
}
