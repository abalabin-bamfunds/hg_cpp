#include <bg/frame/arrow.hpp>

#include <bg/util/static_storage.hpp>

#include <mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>

namespace bg::frame::arrow_details {
namespace {

template <cell_type T>
void construct_arrow_column_literal(const literal::ScalarLiteral &literal, const ScalarRuntimeType &runtime,
                                    void *destination, allocator &memory) {
    const auto &native_runtime = cpp_scalar_runtime_type<Column<T>>();
    auto value = ScalarValue<>::from_literal(native_runtime, literal, memory);
    const auto column = bg::details::scalar_data_value<Column<T>>(bg::details::scalar_data(value));
    auto array = copy_to_arrow(column);
    if (!array.ok())
        throw std::runtime_error{array.status().ToString()};
    static_cast<void>(runtime);
    std::construct_at(static_cast<ArrowColumnReadSource<T> *>(destination), std::move(array).ValueUnsafe());
}

void construct_arrow_column_literal(const literal::ScalarLiteral &literal, const ScalarRuntimeType &runtime,
                                    void *destination, allocator &memory) {
    const auto &column = static_cast<const ColumnType &>(runtime.type());
    if (std::addressof(column.cell_type()) == std::addressof(cpp_scalar_type<i64>()))
        return construct_arrow_column_literal<i64>(literal, runtime, destination, memory);
    if (std::addressof(column.cell_type()) == std::addressof(cpp_scalar_type<double>()))
        return construct_arrow_column_literal<double>(literal, runtime, destination, memory);
    throw std::invalid_argument{"Arrow column literal supports only i64 and double cells"};
}

void construct_arrow_frame_literal(const literal::ScalarLiteral &literal, const ScalarRuntimeType &runtime,
                                   void *destination, allocator &memory) {
    const auto &frame_type = static_cast<const FrameType &>(runtime.type());
    const auto &native_runtime = frame_snapshot_runtime_type(frame_type);
    auto value = ScalarValue<>::from_literal(native_runtime, literal, memory);
    const auto source = bg::details::scalar_data(value);
    const auto native_storage = static_cast<const FrameScalarRuntimeType &>(native_runtime).read_storage(source);
    auto frame = details::frame_view<bg::SCHEMA>(native_storage, source);
    auto table = copy_to_arrow(frame);
    if (!table.ok())
        throw std::runtime_error{table.status().ToString()};
    const auto &schema = static_cast<const Schema &>(frame_type.schema_type());
    std::construct_at(static_cast<ArrowRuntimeFrameStorage *>(destination), std::move(table).ValueUnsafe(), schema);
}

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

struct RuntimeCaches {
    std::mutex runtimes_mutex;
    std::unordered_map<std::type_index, std::unique_ptr<ScalarRuntimeType>> runtimes;
    std::mutex runtime_schema_mutex;
    std::unordered_map<const FrameType *,
                       std::unique_ptr<RuntimeSchemaFrameScalarRuntime>> runtime_schema_runtimes;
};

RuntimeCaches &runtime_caches() {
    static bg::util::static_storage<RuntimeCaches> caches;
    return caches.get();
}

} // namespace

void register_arrow_column_literal_runtime(const ScalarRuntimeType &runtime) {
    bg::frame::details::register_literal_runtime(runtime, ScalarLiteralOperations{&construct_arrow_column_literal});
}

const ScalarRuntimeType &canonical_scalar_runtime(const ScalarType &type, const std::type_info &identity,
                                                  scalar_runtime_factory factory) {
    auto &caches = runtime_caches();
    const auto lock = std::lock_guard{caches.runtimes_mutex};
    const auto key = std::type_index{identity};
    if (const auto entry = caches.runtimes.find(key); entry != caches.runtimes.end()) {
        if (&entry->second->type() != &type)
            throw std::logic_error{"conflicting Arrow scalar runtime identity"};
        return *entry->second;
    }

    auto runtime = factory();
    if (&runtime->type() != &type)
        throw std::logic_error{"Arrow scalar runtime has a non-canonical scalar type"};
    const auto entry = caches.runtimes.emplace(key, std::move(runtime)).first;
    return *entry->second;
}

const ScalarRuntimeType &runtime_schema_frame_scalar_runtime(const FrameType &type) {
    if (!type.is_resolved())
        throw std::invalid_argument{"Arrow frame runtime requires a resolved frame type"};
    static_cast<void>(frame_snapshot_runtime_type(type));

    auto &caches = runtime_caches();
    const auto lock = std::lock_guard{caches.runtime_schema_mutex};
    if (const auto entry = caches.runtime_schema_runtimes.find(std::addressof(type));
        entry != caches.runtime_schema_runtimes.end())
        return *entry->second;

    auto runtime = std::make_unique<RuntimeSchemaFrameScalarRuntime>(type);
    const auto entry = caches.runtime_schema_runtimes.emplace(std::addressof(type), std::move(runtime)).first;
    auto rollback = util::scope_exit{[&]() noexcept { caches.runtime_schema_runtimes.erase(entry); }};
    bg::frame::details::register_literal_runtime(
        *entry->second, ScalarLiteralOperations{&construct_arrow_frame_literal});
    rollback.release();
    return *entry->second;
}

} // namespace bg::frame::arrow_details
