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

struct PreparedWiringCall : ResolvedWiringSignatureMatch {
    // Borrowed from the caller that retains it through candidate selection.
    std::span<const unsigned char> preserved_inputs;

    const Type *argument_type(std::size_t index, const WiringArgument &argument) const noexcept;
    const TimeSeriesType *materialisation_target(std::size_t index) const noexcept;
    bool preserves_input_point(std::size_t index) const noexcept { return preserved_inputs[index] != 0; }
    bool same_concrete_binding(const PreparedWiringCall &other) const;
    double binding_cost(std::span<const WiringArgument> arguments) const;
};

struct WiringCallPreparationResult {
    std::vector<PreparedWiringCall> calls;
    std::exception_ptr completion_error;
};

class WiringCallPreparation {
  public:
    WiringCallPreparation(const Signature &signature, std::span<const WiringArgument> arguments,
                          const TypeMap *seed_type_map = nullptr);

    bool empty() const noexcept { return matches_.empty(); }
    WiringCallPreparationResult complete(std::span<const unsigned char> preserved_inputs,
                                         const WiringArguments &argument_context,
                                         const std::function<void(const TypeMap &)> &completion = {});

  private:
    const Signature *signature_;
    std::vector<WiringSignatureMatch> matches_;
};
void bind_materialisation_forms(GraphWiring &wiring, const WiringTemplate &implementation,
                                std::span<WiringArgument> arguments,
                                std::span<const TimeSeriesType *const> selected_forms);

} // namespace details
} // namespace bg
