#pragma once

#include <catch2/internal/catch_unique_name.hpp>

// MSVC includes the enclosing function name in local type identities across translation units.
#define HG_TEST_UNIQUE_NAME_IMPL(source, name, counter) source##name##counter
#define HG_TEST_UNIQUE_NAME(source, name, counter) HG_TEST_UNIQUE_NAME_IMPL(source, name, counter)

#undef INTERNAL_CATCH_UNIQUE_NAME
#define INTERNAL_CATCH_UNIQUE_NAME(name) HG_TEST_UNIQUE_NAME(HG_TEST_SOURCE_ID, name, __COUNTER__)
