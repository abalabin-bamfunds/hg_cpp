#include <hg/version.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <string>

NB_MODULE(hg_sdk_python_consumer, module) {
    module.def("version", [] { return std::string(hg::version()); });
}
