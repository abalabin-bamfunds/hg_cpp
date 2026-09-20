#pragma once

#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/type/type.hpp>

#include <concepts>
#include <memory>
#include <optional>
#include <typeinfo>
#include <utility>

namespace bg::details {
struct graph_argument_access;
}

namespace bg {

class WiringTemplate;

class GraphArgumentType final : public Type {
  public:
    GraphArgumentType() = default;
    explicit GraphArgumentType(details::GraphWiringStructure structure) : structure_{std::move(structure)} {}
    explicit GraphArgumentType(std::shared_ptr<const WiringTemplate> pattern) : pattern_{std::move(pattern)} {}

    VISITOR_SUPPORT()

    bool is_resolved() const noexcept final { return true; }
    bool is_graph() const noexcept final { return true; }
    const Type &resolve(const TypeMap &) const final { return *this; }

    const details::GraphWiringStructure *structure() const noexcept {
        return structure_ ? &*structure_ : nullptr;
    }

  protected:
    bool do_match(const Type &supplied, TypeMap &) const final;

  private:
    bool matches_pattern(const details::GraphWiringStructure &supplied) const;

    std::optional<details::GraphWiringStructure> structure_;
    std::shared_ptr<const WiringTemplate> pattern_;
};

// An owning wiring-time graph value. Its payload is interpreted only by the
// higher-order operation that constructed it.
class GraphArgument {
  public:
    GraphArgument(details::GraphWiringStructure structure, std::shared_ptr<const void> payload)
        : type_{std::make_shared<GraphArgumentType>(std::move(structure))}, payload_{std::move(payload)},
          payload_type_{&typeid(void)} {}

    template <typename Payload>
        requires(!std::same_as<std::remove_cv_t<Payload>, void>)
    GraphArgument(details::GraphWiringStructure structure, std::shared_ptr<Payload> payload)
        : type_{std::make_shared<GraphArgumentType>(std::move(structure))}, payload_{std::move(payload)},
          payload_type_{&typeid(Payload)} {}

    static const GraphArgumentType &type() noexcept;

    const GraphArgumentType &exact_type() const noexcept { return *type_; }
    const details::GraphWiringStructure &structure() const noexcept { return *type_->structure(); }

  private:
    friend struct details::graph_argument_access;

    std::shared_ptr<const GraphArgumentType> type_;
    std::shared_ptr<const void> payload_;
    const std::type_info *payload_type_;
};

} // namespace bg

namespace bg::details {

struct graph_argument_access {
    static const std::shared_ptr<const GraphArgumentType> &type_owner(const GraphArgument &argument) noexcept {
        return argument.type_;
    }

    template <typename Payload> static std::shared_ptr<const Payload> payload(const GraphArgument &argument) {
        if (*argument.payload_type_ != typeid(Payload)) {
            return {};
        }
        return std::static_pointer_cast<const Payload>(argument.payload_);
    }
};

} // namespace bg::details
