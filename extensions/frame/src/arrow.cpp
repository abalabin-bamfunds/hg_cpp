#include <bg/frame/arrow.hpp>

#include <mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>

namespace bg::frame::arrow_details {
namespace {

auto runtimes_mutex = std::mutex{};
auto runtimes = std::unordered_map<std::type_index, std::unique_ptr<ScalarRuntimeType>>{};

class RuntimeSchemaFrameScalarRuntime final : public FrameScalarRuntimeType {
  public:
    explicit RuntimeSchemaFrameScalarRuntime(const FrameType &type)
        : FrameScalarRuntimeType{type, sizeof(ArrowRuntimeFrameStorage), alignof(ArrowRuntimeFrameStorage), false,
                                 true, false} {}

    FrameReadStorage read_storage(const ScalarData &source) const override {
        return frame_read_storage(*static_cast<const ArrowRuntimeFrameStorage *>(source.data()), *this);
    }

    void read_value(ScalarDataMode mode, std::uintptr_t data, void *destination) const override {
        auto source = ScalarData{*this, data, mode};
        auto value = details::frame_view<bg::SCHEMA>(read_storage(source), source);
        std::construct_at(static_cast<FrameView<bg::SCHEMA> *>(destination), std::move(value));
    }

    const ScalarRuntimeType &runtime_type_for_copy(const ScalarData &) const override { return *this; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<ArrowRuntimeFrameStorage *>(destination));
    }
    void copy_construct(void *destination, const void *source, allocator &) const override {
        std::construct_at(static_cast<ArrowRuntimeFrameStorage *>(destination),
                          *static_cast<const ArrowRuntimeFrameStorage *>(source));
    }
    std::size_t hash(const void *) const noexcept override { return 0; }
    bool equals(const void *, const ScalarData &) const noexcept override { return false; }
    bool equals_same_type(const void *, const ScalarData &) const noexcept override { return false; }
    void destroy(void *value, allocator &) const noexcept override {
        std::destroy_at(static_cast<ArrowRuntimeFrameStorage *>(value));
    }
    bool accepts_input(const ScalarRuntimeType &source) const noexcept override { return &source == this; }

  protected:
    void do_construct_from_input(void *destination, const ScalarData &source, allocator &) const override {
        std::construct_at(static_cast<ArrowRuntimeFrameStorage *>(destination),
                          *static_cast<const ArrowRuntimeFrameStorage *>(source.data()));
    }
    void do_assign_from_input(void *destination, const ScalarData &source) const override {
        auto replacement = *static_cast<const ArrowRuntimeFrameStorage *>(source.data());
        *static_cast<ArrowRuntimeFrameStorage *>(destination) = std::move(replacement);
    }
};

auto runtime_schema_mutex = std::mutex{};
auto runtime_schema_runtimes =
    std::unordered_map<const FrameType *, std::unique_ptr<RuntimeSchemaFrameScalarRuntime>>{};

} // namespace

const ScalarRuntimeType &canonical_scalar_runtime(const ScalarType &type, const std::type_info &identity,
                                                  scalar_runtime_factory factory) {
    const auto lock = std::lock_guard{runtimes_mutex};
    const auto key = std::type_index{identity};
    if (const auto entry = runtimes.find(key); entry != runtimes.end()) {
        if (&entry->second->type() != &type)
            throw std::logic_error{"conflicting Arrow scalar runtime identity"};
        return *entry->second;
    }

    auto runtime = factory();
    if (&runtime->type() != &type)
        throw std::logic_error{"Arrow scalar runtime has a non-canonical scalar type"};
    const auto entry = runtimes.emplace(key, std::move(runtime)).first;
    return *entry->second;
}

const ScalarRuntimeType &runtime_schema_frame_scalar_runtime(const FrameType &type) {
    if (!type.is_resolved())
        throw std::invalid_argument{"Arrow frame runtime requires a resolved frame type"};
    static_cast<void>(frame_snapshot_runtime_type(type));

    const auto lock = std::lock_guard{runtime_schema_mutex};
    if (const auto entry = runtime_schema_runtimes.find(std::addressof(type));
        entry != runtime_schema_runtimes.end())
        return *entry->second;

    auto runtime = std::make_unique<RuntimeSchemaFrameScalarRuntime>(type);
    const auto entry = runtime_schema_runtimes.emplace(std::addressof(type), std::move(runtime)).first;
    return *entry->second;
}

} // namespace bg::frame::arrow_details
