#pragma once

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
    WiredNode(const NodeTemplate &node_template, ResolvedSignature signature, std::vector<std::size_t> input_sources,
              std::vector<WiredScalar> scalar_values, std::vector<const ScalarType *> state_types,
              const WiringPath &path)
        : node_template_(&node_template), signature_(std::move(signature)), input_sources_(std::move(input_sources)),
          scalar_values_(std::move(scalar_values)), state_types_(std::move(state_types)), path_(&path) {}

    WiredNode(const WiredNode &) = delete;
    WiredNode &operator=(const WiredNode &) = delete;
    WiredNode(WiredNode &&) noexcept = default;
    WiredNode &operator=(WiredNode &&) = delete;

    [[nodiscard]] const NodeTemplate &node_template() const noexcept { return *node_template_; }

    [[nodiscard]] const ResolvedSignature &signature() const noexcept { return signature_; }

    [[nodiscard]] std::span<const std::size_t> input_sources() const noexcept { return input_sources_; }

    [[nodiscard]] std::span<const WiredScalar> scalar_values() const noexcept { return scalar_values_; }

    [[nodiscard]] std::span<const ScalarType *const> state_types() const noexcept { return state_types_; }

    [[nodiscard]] const WiringPath &path() const noexcept { return *path_; }

  private:
    const NodeTemplate *node_template_;
    ResolvedSignature signature_;
    std::vector<std::size_t> input_sources_;
    std::vector<WiredScalar> scalar_values_;
    std::vector<const ScalarType *> state_types_;
    const WiringPath *path_;
};

} // namespace bg
