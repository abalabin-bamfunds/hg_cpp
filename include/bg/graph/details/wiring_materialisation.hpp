#pragma once

#include <bg/graph/wiring_argument.hpp>
#include <bg/type/signature.hpp>

#include <compare>
#include <cstddef>
#include <exception>
#include <functional>
#include <span>
#include <vector>

namespace bg {

class GraphWiring;
class WiringTemplate;

namespace details {

bool is_union_wiring_parameter(const Signature &signature, std::size_t index);
bool preserves_union_wiring_inputs(const WiringTemplate &implementation);
bool preserves_union_wiring_input(const WiringTemplate &implementation, std::size_t index);

struct PreparedWiringCall : ResolvedWiringSignatureMatch {
    // Borrowed; the declaration must outlive this prepared call.
    const Signature *declared_signature;
    bool preserve_union_inputs;

    const Type *argument_type(std::size_t index, const WiringArgument &argument) const noexcept;
    const TimeSeriesType *materialisation_target(std::size_t index) const noexcept;
    bool same_concrete_binding(const PreparedWiringCall &other) const noexcept;
};

struct WiringCallPreparationResult {
    std::vector<PreparedWiringCall> calls;
    std::exception_ptr completion_error;
};

class WiringCallPreparation {
  public:
    WiringCallPreparation(const Signature &signature, std::span<const WiringArgument> arguments,
                          bool preserve_union_inputs, const TypeMap *seed_type_map = nullptr);

    bool empty() const noexcept { return matches_.empty(); }
    WiringCallPreparationResult complete(const WiringArguments &argument_context,
                                         const std::function<void(const TypeMap &)> &completion = {});

  private:
    const Signature *signature_;
    bool preserve_union_inputs_;
    std::vector<WiringSignatureMatch> matches_;
};

std::partial_ordering compare_wiring_reference_affinity(const ResolvedSignature &left,
                                                        const ResolvedSignature &right,
                                                        std::span<const Type *const> left_arguments,
                                                        std::span<const Type *const> right_arguments,
                                                        std::size_t parameter_count) noexcept;
void bind_materialisation_forms(GraphWiring &wiring, const WiringTemplate &implementation,
                                std::span<WiringArgument> arguments,
                                std::span<const TimeSeriesType *const> selected_forms);

} // namespace details
} // namespace bg
