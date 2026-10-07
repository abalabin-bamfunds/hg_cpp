#pragma once

#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/type/type.hpp>

#include <concepts>
#include <compare>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

namespace bg {

class WiringTemplate;

class GraphArgumentType final : public Type {
  public:
    GraphArgumentType() = default;
    GraphArgumentType(std::vector<const Type *> parameters, const TimeSeriesType *output);

    VISITOR_SUPPORT()

    bool is_resolved() const noexcept final;
    bool is_graph() const noexcept final { return true; }
    const Type &resolve(const TypeMap &) const final;

    bool has_signature() const noexcept { return signature_; }
    std::span<const Type *const> parameters() const noexcept { return parameters_; }
    const TimeSeriesType *output() const noexcept { return output_; }

  protected:
    bool do_match(const Type &supplied, TypeMap &) const final;

  private:
    bool match_signature(const GraphArgumentType &supplied, TypeMap &type_map) const;

    bool signature_{};
    std::vector<const Type *> parameters_;
    const TimeSeriesType *output_{};
};

// Interns a process-owned graph type. Every parameter and output type must have process lifetime.
const GraphArgumentType &graph_argument_type(std::span<const Type *const> parameters,
                                             const TimeSeriesType *output);
const GraphArgumentType &graph_argument_type(const details::GraphWiringStructure &structure);

class GraphArgumentConstraint final {
  public:
    GraphArgumentConstraint() = default;
    explicit GraphArgumentConstraint(details::GraphWiringStructure structure);
    explicit GraphArgumentConstraint(std::vector<details::GraphWiringStructure> structures);
    explicit GraphArgumentConstraint(std::shared_ptr<const WiringTemplate> pattern,
                                     const void *pattern_identity = nullptr);

    bool constrained() const noexcept;
    bool matches(const class GraphArgument &argument) const;
    bool same_constraint(const GraphArgumentConstraint &other) const;
    std::partial_ordering compare_specificity(const GraphArgumentConstraint &other) const;

  private:
    bool matches_pattern(const details::GraphWiringStructure &supplied) const;

    using StructureConstraint =
        std::variant<std::monostate, details::GraphWiringStructure,
                     std::vector<details::GraphWiringStructure>>;

    StructureConstraint structures_;
    std::shared_ptr<const WiringTemplate> pattern_;
    const void *pattern_identity_{};
};

using GraphArgumentStructures =
    std::variant<details::GraphWiringStructure, std::vector<details::GraphWiringStructure>>;

// An owning wiring-time graph value. Its structure and opaque payload have separate lifetimes.
class GraphArgument {
  public:
    GraphArgument(details::GraphWiringStructure structure, std::shared_ptr<const void> payload)
        : GraphArgument{std::move(structure), std::move(payload), &typeid(void)} {}

    GraphArgument(std::vector<details::GraphWiringStructure> structures,
                  std::shared_ptr<const void> payload)
        : GraphArgument{std::move(structures), std::move(payload), &typeid(void)} {}

    template <typename Payload>
        requires(!std::same_as<std::remove_cv_t<Payload>, void>)
    GraphArgument(details::GraphWiringStructure structure, std::shared_ptr<Payload> payload)
        : GraphArgument{std::move(structure), std::shared_ptr<const void>{std::move(payload)}, &typeid(Payload)} {}

    template <typename Payload>
        requires(!std::same_as<std::remove_cv_t<Payload>, void>)
    GraphArgument(std::vector<details::GraphWiringStructure> structures,
                  std::shared_ptr<Payload> payload)
        : GraphArgument{std::move(structures), std::shared_ptr<const void>{std::move(payload)}, &typeid(Payload)} {}

    static const GraphArgumentType &type() noexcept;

    const GraphArgumentType &exact_type() const noexcept { return *type_; }
    const details::GraphWiringStructure &structure() const;
    const GraphArgumentStructures &structures() const noexcept { return structures_; }

    template <typename Payload> std::shared_ptr<const Payload> payload() const {
        if (*payload_type_ != typeid(Payload)) {
            return {};
        }
        return std::static_pointer_cast<const Payload>(payload_);
    }

  private:
    GraphArgument(details::GraphWiringStructure structure, std::shared_ptr<const void> payload,
                  const std::type_info *payload_type);
    GraphArgument(std::vector<details::GraphWiringStructure> structures, std::shared_ptr<const void> payload,
                  const std::type_info *payload_type);

    GraphArgumentStructures structures_;
    const GraphArgumentType *type_{};
    std::shared_ptr<const void> payload_;
    const std::type_info *payload_type_;
};

} // namespace bg

namespace bg::details {

template <typename T>
concept graph_argument_facade = requires(const std::remove_cvref_t<T> &value) {
    { value.graph_argument() } -> std::same_as<const GraphArgument &>;
    { std::remove_cvref_t<T>::type() } -> std::same_as<const GraphArgumentType &>;
};

} // namespace bg::details
