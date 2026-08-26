#pragma once

#include <bg/type/time_series_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

struct NodeParameter {
    util::symbol name;
    const Type *type;
};

struct VariableResolver {
    const Variable *variable;
    std::function<TypeBinding(const TypeMap &)> resolve;
};

struct ResolvedNodeParameter {
    util::symbol name;
    const Type *type;
};

struct ResolvedNodeSignature {
    util::symbol name;
    std::vector<ResolvedNodeParameter> parameters;
    const TimeSeriesType *output;
};

class NodeSignature {
  public:
    NodeSignature(util::symbol name,
                  std::vector<NodeParameter> parameters,
                  const TimeSeriesType &output,
                  std::vector<VariableResolver> resolvers = {})
        : name_(name),
          parameters_(std::move(parameters)),
          output_(&output),
          resolvers_(std::move(resolvers)) {}

    [[nodiscard]] ResolvedNodeSignature
    resolve(std::span<const Type *const> arguments) const;

  private:
    util::symbol name_;
    std::vector<NodeParameter> parameters_;
    const TimeSeriesType *output_;
    std::vector<VariableResolver> resolvers_;
};

} // namespace bg
