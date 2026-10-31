#pragma once

#include <bg/type/details/static_registry_entry.hpp>
#include <bg/util/symbol.hpp>

namespace bg {

class Variable {
  public:
    explicit Variable(util::symbol name) : name_(name), registry_entry_{*this} {}
    Variable(const Variable &) = delete;
    Variable &operator=(const Variable &) = delete;
    Variable(Variable &&) = delete;
    Variable &operator=(Variable &&) = delete;
    virtual ~Variable();

    util::symbol name() const noexcept { return name_; }
    // Engine registration hook for variables with process-static storage.
    details::StaticRegistryEntry<Variable> &registry_entry() const noexcept { return registry_entry_; }

  private:
    util::symbol name_;
    mutable details::StaticRegistryEntry<Variable> registry_entry_;
};

} // namespace bg
