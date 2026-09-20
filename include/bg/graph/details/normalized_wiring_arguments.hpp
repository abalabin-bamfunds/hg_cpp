#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/details/wiring_argument_descriptor.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/type/signature.hpp>

#include <memory>
#include <span>
#include <vector>

namespace bg::details {

struct NormalizedWiringArguments {
    std::vector<WiringArgument> arguments;
    std::vector<WiringArgumentDescriptor> descriptors;
    std::vector<std::unique_ptr<WiringRecord>> probe_records;
};

[[nodiscard]] NormalizedWiringArguments normalize_wiring_arguments(const Signature &signature,
                                                                    std::span<const WiringArgument> arguments,
                                                                    allocator &memory);
[[nodiscard]] NormalizedWiringArguments probe_wiring_arguments(const Signature &signature,
                                                                std::span<const WiringArgument> arguments);

} // namespace bg::details
