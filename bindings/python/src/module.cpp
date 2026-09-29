#include <bg/python/module.hpp>

NB_MODULE(_bg, module) {
    module.doc() = "Python bindings for bg";
    bg::python::bind_core(module);
}
