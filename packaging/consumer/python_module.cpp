#include <bg/version.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <string>

NB_MODULE(bg_sdk_python_consumer, module) {
    module.def("version", [] { return std::string(bg::version()); });
}
