#pragma once

#include <bg/graph/wired_input.hpp>
#include <bg/graph/wired_scalar.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace bg {

class NodeTemplate;
struct WiringPath;

class WiredNode {
  public:
    WiredNode(const NodeTemplate &node_template, ResolvedSignature signature, std::vector<WiredInput> inputs,
              std::vector<WiredScalar> scalar_values, std::vector<const ScalarType *> state_types,
              std::vector<const TimeSeriesType *> dynamic_reference_input_types, const WiringPath &path)
        : node_template_(&node_template), signature_(std::move(signature)), inputs_(std::move(inputs)),
          scalar_values_(std::move(scalar_values)), state_types_(std::move(state_types)),
          dynamic_reference_input_types_(std::move(dynamic_reference_input_types)), path_(&path) {}

    WiredNode(const WiredNode &) = delete;
    WiredNode &operator=(const WiredNode &) = delete;
    WiredNode(WiredNode &&) noexcept = default;
    WiredNode &operator=(WiredNode &&) = delete;

    const NodeTemplate &node_template() const noexcept { return *node_template_; }

    const ResolvedSignature &signature() const noexcept { return signature_; }

    std::span<const WiredInput> inputs() const noexcept { return inputs_; }

    std::span<const WiredScalar> scalar_values() const noexcept { return scalar_values_; }

    std::span<const ScalarType *const> state_types() const noexcept { return state_types_; }

    std::span<const TimeSeriesType *const> dynamic_reference_input_types() const noexcept {
        return dynamic_reference_input_types_;
    }

    const WiringPath &path() const noexcept { return *path_; }

  private:
    const NodeTemplate *node_template_;
    ResolvedSignature signature_;
    std::vector<WiredInput> inputs_;
    std::vector<WiredScalar> scalar_values_;
    std::vector<const ScalarType *> state_types_;
    std::vector<const TimeSeriesType *> dynamic_reference_input_types_;
    const WiringPath *path_;
};

} // namespace bg
