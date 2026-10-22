#pragma once

namespace bg {
class WiringError;
}

namespace bg::python::details {

bool has_python_error(const WiringError &error);
[[noreturn]] void rethrow_python_error(const WiringError &error);

} // namespace bg::python::details
