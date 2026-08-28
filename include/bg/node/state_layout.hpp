#pragma once

#include <cstddef>
#include <vector>

namespace bg {

class ScalarStorageRepresentation;

struct NodeStateLayout {
    std::size_t size{};
    std::size_t alignment{1};
    std::vector<std::size_t> field_offsets;
    std::vector<const ScalarStorageRepresentation *> generic_storage_representations;
};

} // namespace bg
