#pragma once

#include <vector>

namespace bg {

class ScalarType;
class ResolvedHierarchicalScalarType;

namespace details {

enum class HashRequirement {
    set_item,
    dictionary_key,
    time_series_set_item,
    time_series_dictionary_key,
};

struct PendingHashRequirement {
    const ScalarType *type;
    HashRequirement requirement;
};

// Tracks incomplete semantic descriptors during single-threaded type registration.
class SemanticTypeRegistration {
  public:
    SemanticTypeRegistration();
    ~SemanticTypeRegistration();

    SemanticTypeRegistration(const SemanticTypeRegistration &) = delete;
    SemanticTypeRegistration &operator=(const SemanticTypeRegistration &) = delete;

    static SemanticTypeRegistration *current() noexcept;

    void add_hierarchical_type(ResolvedHierarchicalScalarType &type);
    void require_hash(const ScalarType &type, HashRequirement requirement);
    void complete();

  private:
    std::vector<ResolvedHierarchicalScalarType *> hierarchical_types_;
    std::vector<PendingHashRequirement> hash_requirements_;
};

} // namespace details
} // namespace bg
