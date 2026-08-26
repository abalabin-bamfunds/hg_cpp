find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_bg MODULE src/module.cpp)
target_link_libraries(_bg PRIVATE bg::bg)
target_compile_features(_bg PRIVATE cxx_std_23)
