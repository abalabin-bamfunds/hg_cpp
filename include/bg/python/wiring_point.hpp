#pragma once

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/python/export.hpp>

#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace bg {
class NodeTemplate;
}

namespace bg::python {

class BG_PYTHON_API PythonWiringLifetime {
  public:
    explicit PythonWiringLifetime(const GraphWiring &wiring) noexcept : wiring_{&wiring} {}

    void end() noexcept { active_ = false; }

    bool is_active_for(const GraphWiring &wiring) const noexcept { return active_ && wiring_ == &wiring; }

    const NodeTemplate *node_template(const void *identity) const noexcept {
        const auto found = node_templates_.find(identity);
        return found == node_templates_.end() ? nullptr : found->second;
    }

    void add_node_template(const void *identity, const NodeTemplate &node_template) {
        node_templates_.emplace(identity, &node_template);
    }

    void check(const GraphWiring &wiring) const {
        if (!is_active_for(wiring)) {
            throw std::runtime_error{"a Python wiring point was used outside its graph construction"};
        }
    }

    void check() const {
        if (!active_) {
            throw std::runtime_error{"a Python wiring point was used outside its graph construction"};
        }
    }

  private:
    const GraphWiring *wiring_;
    std::unordered_map<const void *, const NodeTemplate *> node_templates_;
    bool active_{true};
};

class BG_PYTHON_API PythonWiringPoint {
  public:
    PythonWiringPoint(WiringPoint point, std::shared_ptr<PythonWiringLifetime> lifetime) noexcept
        : point_{point}, lifetime_{std::move(lifetime)} {}

    WiringPoint get(const GraphWiring &wiring) const {
        lifetime_->check(wiring);
        return point_;
    }

    bool has_output() const {
        lifetime_->check();
        return point_.has_output();
    }

    const TimeSeriesType &type() const {
        lifetime_->check();
        return point_.type();
    }

  private:
    WiringPoint point_;
    std::shared_ptr<PythonWiringLifetime> lifetime_;
};

class BG_PYTHON_API PythonWiringScope {
  public:
    explicit PythonWiringScope(const GraphWiring &wiring);
    ~PythonWiringScope();

    PythonWiringScope(const PythonWiringScope &) = delete;
    PythonWiringScope &operator=(const PythonWiringScope &) = delete;

    const std::shared_ptr<PythonWiringLifetime> &lifetime() const noexcept { return lifetime_; }
    static std::shared_ptr<PythonWiringLifetime> current();

  private:
    std::shared_ptr<PythonWiringLifetime> lifetime_;
    std::shared_ptr<PythonWiringLifetime> previous_lifetime_;
    bool owner_{};
};

} // namespace bg::python
