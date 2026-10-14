#pragma once

#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/node_call_identity.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/graph/wired_input.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bg {

class GraphWiring;
class Signature;
class TimeSeriesRuntimeType;
class TimeSeriesType;
namespace details {

struct MaterialisationDependency {
    WiringPoint point;
    const TimeSeriesType *target_type;
    util::symbol operation;
    std::string path;
    std::optional<util::symbol> parameter;
    std::optional<item_id_t> item;
};

struct MaterialisationPreparation {
    std::vector<WiringArgument> arguments;
    std::vector<const TimeSeriesType *> input_targets;
    std::vector<MaterialisationDependency> dependencies;
};

// Retains wiring-time behavior that is not represented by the semantic time series type.
class WiringRecord {
  public:
    static constexpr auto no_output = static_cast<std::size_t>(-1);

    virtual ~WiringRecord() = default;

    GraphWiring &wiring() const noexcept { return *graph_; }
    const TimeSeriesType &type() const noexcept { return *type_; }

    // Associates a new record with its graph before exposing its wiring point.
    void initialize(GraphWiring &wiring, std::size_t output = no_output) noexcept {
        graph_ = &wiring;
        output_ = output;
        graph_owned_ = true;
    }

    void set_wired_output_index(std::size_t output) noexcept { output_ = output; }

    void initialize_overlay(GraphWiring &wiring) noexcept {
        graph_ = &wiring;
        output_ = no_output;
        graph_owned_ = false;
    }

    bool has_wired_output() const noexcept { return output_ != no_output; }
    bool graph_owned() const noexcept { return graph_owned_; }
    virtual bool has_default_materialisation() const noexcept { return true; }
    virtual std::optional<WiredInput> input_binding() const noexcept { return std::nullopt; }
    virtual std::optional<NodeCallIdentity> node_call_identity() const noexcept { return std::nullopt; }
    virtual const WiringRecord *source_identity() const noexcept { return this; }
    std::size_t wired_output_index() const noexcept { return output_; }
    // A record can bind an input directly to storage that has no wired output, such as an inherited slot.
    virtual std::optional<WiredInput> input_binding(const TimeSeriesType &) const noexcept { return std::nullopt; }

    // Returns a compatible point in this graph; it may still be lazy. GraphWiring completes lazy results to a wired
    // output or direct input binding and validates their graph and type before returning them to a consumer.
    virtual WiringPoint materialize(const TimeSeriesType &requested_type) const = 0;
    virtual void add_materialisation_option(const TimeSeriesType &target_type, double cost,
                                            Signature input_signature,
                                            WiringMaterialisationBuilder builder) const;
    virtual std::vector<WiringMaterialisationOption> materialisation_options() const;
    virtual std::optional<WiringErrorContext>
    materialisation_context(const TimeSeriesType &target_type) const;
    // Reports child demands without expanding this record. Lazy records must report the same child targets their
    // materialize() operation consumes; the collector uses them to discover upstream requirements first.
    virtual MaterialisationPreparation prepare_materialisation(const TimeSeriesType &target_type) const;
    virtual WiringPoint expand_materialisation(const TimeSeriesType &target_type,
                                               std::span<const WiringArgument> arguments,
                                               std::span<const TimeSeriesType *const> input_targets) const;

    static WiringPoint point(const WiringRecord &record) noexcept;

  protected:
    explicit WiringRecord(const TimeSeriesType &type) noexcept : type_{&type} {}

  private:
    GraphWiring *graph_{};
    const TimeSeriesType *type_;
    // A wired output index or no_output.
    std::size_t output_;
    bool graph_owned_{};
};

WiringPoint add_wiring_record(std::unique_ptr<WiringRecord> record);
WiringPoint add_item_wiring_record(WiringPoint parent, item_id_t item,
                                   std::unique_ptr<WiringRecord> record);

} // namespace details
} // namespace bg
