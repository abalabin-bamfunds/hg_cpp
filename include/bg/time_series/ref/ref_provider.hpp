#pragma once

#include <bg/graph/output_notification.hpp>

#include <cstdint>
#include <memory_resource>
#include <vector>

namespace bg {

class RuntimeGraph;
class ScalarRuntimeType;
class TimeSeriesType;

namespace details {

class ScalarData;
struct ReferenceStorage;
class DictionaryObservers;

class ReferenceRepresentation {
  public:
    virtual ~ReferenceRepresentation() = default;

    [[nodiscard]] virtual const ReferenceStorage &source() const noexcept = 0;
    [[nodiscard]] virtual ReferenceStorage &reference() noexcept = 0;
    virtual void source_changed(OutputNotificationContext &context, OutputChange change) = 0;
    virtual void destroy(std::pmr::memory_resource &memory) noexcept = 0;
};

class ReferenceProvider {
  public:
    ReferenceProvider(RuntimeGraph &owner, std::pmr::memory_resource &memory, ReferenceStorage &root);
    ~ReferenceProvider();

    ReferenceProvider(const ReferenceProvider &) = delete;
    ReferenceProvider &operator=(const ReferenceProvider &) = delete;

    [[nodiscard]] RuntimeGraph &owner() const noexcept { return *owner_; }
    [[nodiscard]] std::pmr::memory_resource &memory() const noexcept { return *memory_; }
    [[nodiscard]] ReferenceStorage &representation(ReferenceStorage &source, const TimeSeriesType &requested_type);
    void add_dictionary_key_subscriber(ReferenceStorage &source, const ScalarData &key, RuntimeGraph &graph,
                                       std::uint32_t input_offset, std::uint32_t node_index);
    void remove_dictionary_key_subscriber(ReferenceStorage &source, const ScalarData &key, RuntimeGraph &graph,
                                          std::uint32_t input_offset) noexcept;
    void source_changed(OutputNotificationContext &context, OutputChange change);

  private:
    RuntimeGraph *owner_;
    std::pmr::memory_resource *memory_;
    ReferenceStorage *root_;
    std::pmr::vector<ReferenceRepresentation *> representations_;
    DictionaryObservers *dictionary_observers_{};
};

} // namespace details
} // namespace bg
