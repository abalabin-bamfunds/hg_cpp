#pragma once

namespace hg {
class WiringError;
}

namespace hg::python::details {

bool has_python_error(const WiringError &error);
[[noreturn]] void rethrow_python_error(const WiringError &error);

} // namespace hg::python::details
