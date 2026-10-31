#pragma once

#include <nanobind/nanobind.h>

namespace bg::python::details {

void register_delta_types(nanobind::handle set_delta_type, nanobind::handle removed_type,
                          nanobind::handle remove_sentinel, nanobind::handle invalid_sentinel);
bool is_remove(nanobind::handle value);
bool is_invalid(nanobind::handle value);
bool is_set_delta(nanobind::handle value);
bool is_removed(nanobind::handle value);
nanobind::handle remove_sentinel();
nanobind::handle invalid_sentinel();
nanobind::object make_set_delta(nanobind::handle added, nanobind::handle removed);

} // namespace bg::python::details
