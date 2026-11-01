#include <hg/python/module.hpp>

NB_MODULE(_hg, module) {
    module.doc() = "Python bindings for hg";
    hg::python::bind_core(module);
}
