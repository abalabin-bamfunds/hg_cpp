#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/details/wiring_argument_descriptor.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/type/signature.hpp>

#include <span>
#include <vector>

namespace bg::details {

class WiringRecordOverlay;

struct NormalizedWiringArguments {
    std::vector<WiringArgument> arguments;
    std::vector<WiringArgumentDescriptor> descriptors;
};

[[nodiscard]] NormalizedWiringArguments normalize_wiring_arguments(const Signature &signature,
                                                                   std::span<const WiringArgument> arguments,
                                                                   allocator &memory,
                                                                   const Signature *parameter_aliases = nullptr);
// Records must outlive the returned arguments.
[[nodiscard]] NormalizedWiringArguments probe_wiring_arguments(const Signature &signature,
                                                               std::span<const WiringArgument> arguments,
                                                               WiringRecordOverlay &records,
                                                               const Signature *parameter_aliases = nullptr);

} // namespace bg::details
