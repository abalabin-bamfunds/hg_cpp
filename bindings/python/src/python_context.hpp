#pragma once

#include <nanobind/nanobind.h>

namespace hg::python::details {

void enter_context(nanobind::handle point_object);
void exit_context(nanobind::handle point_object);
void activate_contexts();

} // namespace hg::python::details
