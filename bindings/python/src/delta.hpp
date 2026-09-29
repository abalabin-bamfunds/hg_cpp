#pragma once

#include <nanobind/nanobind.h>

namespace bg::python::details {

void register_delta_types(nanobind::handle set_delta_type, nanobind::handle tsd_delta_type,
                          nanobind::handle remove_sentinel);
bool is_remove(nanobind::handle value);
bool is_set_delta(nanobind::handle value);
bool is_tsd_delta(nanobind::handle value);
nanobind::object make_set_delta(nanobind::handle added, nanobind::handle removed);
nanobind::object make_tsd_delta(nanobind::handle removed, nanobind::handle modified);

} // namespace bg::python::details
