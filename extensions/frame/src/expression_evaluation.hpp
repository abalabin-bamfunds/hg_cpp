#pragma once

#include <hg/frame/expression.hpp>
#include <hg/frame/tsc.hpp>
#include <hg/graph.hpp>
#include <hg/scalar/opaque.hpp>
#include <hg/util/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <memory_resource>
#include <vector>

namespace hg::frame::details {

inline constexpr auto expression_batch_rows = std::size_t{128};

template <template <typename, auto...> typename Field>
using COLUMNS = BundleSchemaVariableDeclaration<"FRAME_EXPRESSION_COLUMNS", Field>;
template <template <typename, auto...> typename Field>
using PARAMETER_INPUTS = BundleSchemaVariableDeclaration<"FRAME_EXPRESSION_PARAMETER_INPUTS", Field>;
template <template <typename, auto...> typename Field>
using PARAMETERS = BundleSchemaVariableDeclaration<"FRAME_EXPRESSION_PARAMETERS", Field>;
using PARAMETER_SCALAR = ScalarVariable<"FRAME_EXPRESSION_PARAMETER_SCALAR">;
template <template <typename, auto...> typename Field> struct EmptyParameterSchema {};

using CompiledExpressionOwner = std::shared_ptr<const struct CompiledExpression>;

struct ColumnInputAdapter {
    ErasedColumnSeriesView (*read)(const TimeSeriesInputData &);
    bool (*bind)(const void *, const TimeSeriesInputData &);
    RowChanges (*changes)(const TimeSeriesInputData &);
};

struct ColumnBinding {
    std::size_t field_index{};
    ColumnInputAdapter adapter{};
};

struct ParameterBinding {
    std::size_t field_index{};
    std::size_t offset{};
    std::size_t size{};
    std::size_t alignment{};
    void (*read)(Scalar<>, void *){};
};

struct Operand {
    enum class Source { column, parameter, instruction };

    std::size_t index{};
    Source source{Source::column};
};

struct Instruction {
    std::array<Operand, 2> operands;
    std::size_t operand_count{};
    ScalarValue<> callback;
    const ColumnBlockFunctionAdapter *adapter{};
    std::size_t result_offset{};
};

struct CompiledExpression {
    Operand result;
    std::vector<Instruction> instructions;
    std::vector<ColumnBinding> columns;
    std::vector<ParameterBinding> parameters;
    std::size_t parameter_storage_size{};
    std::size_t instruction_storage_size{};

    template <typename Function>
    void for_each_result_batch(std::span<const ErasedColumnSeriesView> views,
                               std::span<const void *const> parameters, const RowSet &rows,
                               std::pmr::memory_resource &memory, Function &&function) const {
        constexpr auto max_rows = expression_batch_rows;
        const auto validity_capacity = (max_rows + 7) / 8;
        const auto value_bytes = instruction_storage_size;
        auto *operation_values = value_bytes == 0 ? nullptr : memory.allocate(value_bytes, alignof(std::max_align_t));
        auto release_operation_values = util::scope_exit{[&]() noexcept {
            if (operation_values != nullptr)
                memory.deallocate(operation_values, value_bytes, alignof(std::max_align_t));
        }};
        auto operation_validity =
            std::pmr::vector<std::uint8_t>{instructions.size() * validity_capacity, 0, &memory};
        auto operation_batches = std::pmr::vector<ColumnBatch<>>{instructions.size(), &memory};

        for_each_aligned_column_batch(
            views, rows, memory,
            [&](std::span<const row_id_t> ids, std::span<const ColumnBatch<>> batches) {
                for (auto offset = std::size_t{}; offset < ids.size(); offset += max_rows) {
                    const auto count = std::min(max_rows, ids.size() - offset);
                    const auto batch_ids = ids.subspan(offset, count);
                    const auto validity_bytes = (count + 7) / 8;
                    for (auto operation = std::size_t{}; operation != instructions.size(); ++operation) {
                        const auto &instruction = instructions[operation];
                        auto operands = std::array<ColumnOperandBatch<>, 2>{};
                        for (auto operand_index = std::size_t{}; operand_index != instruction.operand_count;
                             ++operand_index) {
                            const auto operand = instruction.operands[operand_index];
                            switch (operand.source) {
                            case Operand::Source::column: {
                                const auto &batch = batches[operand.index];
                                operands[operand_index] =
                                    ColumnOperandBatch<>{batch.subspan(offset, count)};
                                break;
                            }
                            case Operand::Source::parameter:
                                operands[operand_index] = ColumnOperandBatch<>{parameters[operand.index]};
                                break;
                            case Operand::Source::instruction: {
                                const auto &batch = operation_batches[operand.index];
                                operands[operand_index] = ColumnOperandBatch<>{batch};
                                break;
                            }
                            }
                        }

                        auto *values = static_cast<std::byte *>(operation_values) + instruction.result_offset;
                        auto *validity = operation_validity.data() + operation * validity_capacity;
                        instruction.adapter->call(
                            instruction.callback.view(), batch_ids,
                            std::span<const ColumnOperandBatch<>>{operands.data(), instruction.operand_count},
                            MutableColumnBatch<>{values, count,
                                                 std::span<std::uint8_t>{validity, validity_bytes}});
                        operation_batches[operation] = ColumnBatch<>{
                            0, count, values, 0,
                            instruction.adapter->packed_result ? 1 : instruction.adapter->result_size * 8,
                            ColumnBatchValidity::from_bitmap(validity, 0)};
                    }

                    const auto result_batch = [&] {
                        switch (result.source) {
                        case Operand::Source::column:
                            return batches[result.index].subspan(offset, count);
                        case Operand::Source::instruction:
                            return operation_batches[result.index];
                        case Operand::Source::parameter:
                            std::unreachable();
                        }
                        std::unreachable();
                    }();
                    function(batch_ids, result_batch);
                }
            });
    }
};

struct PreparedColumns {
    PreparedColumns(const TimeSeriesInputData &inputs, std::span<const ColumnBinding> bindings,
                    std::pmr::memory_resource *memory)
        : views{memory}, changes{memory} {
        views.reserve(bindings.size());
        changes.reserve(bindings.size());
        for (const auto &binding : bindings) {
            const auto data = inputs.item(static_cast<item_id_t>(binding.field_index));
            views.push_back(binding.adapter.read(data));
            changes.push_back(binding.adapter.changes(data));
        }
        const auto &rows = views.front().visible_rows();
        for (auto index = std::size_t{1}; index != views.size(); ++index)
            if (views[index].visible_rows() != rows)
                throw std::invalid_argument{"column expression inputs must share one visible row selection"};
    }

    std::pmr::vector<ErasedColumnSeriesView> views;
    std::pmr::vector<RowChanges> changes;
};

struct PreparedExpressionInputs {
    struct ParameterStorageBlock {
        alignas(std::max_align_t) std::byte bytes[sizeof(std::max_align_t)];
    };

    PreparedExpressionInputs(const CompiledExpression &expression, const TimeSeriesInputData &columns,
                             const TimeSeriesInputData &parameters,
                             std::pmr::memory_resource &memory)
        : columns{columns, std::span{expression.columns}, &memory},
          parameter_storage{&memory},
          parameter_addresses{std::pmr::polymorphic_allocator<const void *>{&memory}} {
        parameter_storage.resize((expression.parameter_storage_size + sizeof(std::max_align_t) - 1) /
                                 sizeof(std::max_align_t));
        parameter_addresses.reserve(expression.parameters.size());
        const auto parameter_bundle = In<TS<Bundle<PARAMETERS>>>{parameters}.value_view();
        for (auto index = std::size_t{}; index != expression.parameters.size(); ++index) {
            const auto &binding = expression.parameters[index];
            auto *value = parameter_storage[binding.offset / sizeof(ParameterStorageBlock)].bytes +
                          binding.offset % sizeof(ParameterStorageBlock);
            const auto parameter = parameter_bundle[binding.field_index];
            binding.read(Scalar<>::from(parameter), value);
            parameter_addresses.push_back(value);
        }
    }

    PreparedExpressionInputs(const PreparedExpressionInputs &) = delete;
    PreparedExpressionInputs &operator=(const PreparedExpressionInputs &) = delete;

    PreparedColumns columns;
    std::pmr::vector<ParameterStorageBlock> parameter_storage;
    std::pmr::vector<const void *> parameter_addresses;
};

struct EvaluationRows {
    EvaluationRows(const RowSet &visible_rows, std::pmr::memory_resource *memory)
        : ids{memory}, requested{visible_rows} {}

    void select_changes(std::span<const RowChanges> changes) {
        auto bitmaps = std::pmr::vector<const util::index<> *>{ids.get_allocator().resource()};
        for (const auto &change : changes) {
            if (change.added != nullptr)
                bitmaps.push_back(change.added);
            if (change.modified != nullptr)
                bitmaps.push_back(change.modified);
        }
        util::index<>::for_each_set_union(bitmaps, [&](row_id_t id) { ids.push_back(id); });
        requested = RowSet{
            .context = std::addressof(ids),
            .native_rows = requested.native_rows,
            .identity = requested.identity,
            .size = [](const void *context) noexcept {
                return static_cast<const std::pmr::vector<row_id_t> *>(context)->size();
            },
            .contains = [](const void *context, row_id_t id) noexcept {
                const auto &ids = *static_cast<const std::pmr::vector<row_id_t> *>(context);
                return std::ranges::binary_search(ids, id);
            },
            .id_at = [](const void *context, std::size_t index) {
                return (*static_cast<const std::pmr::vector<row_id_t> *>(context))[index];
            },
            .for_each = [](const void *context, void *target, RowSet::visit_function visit) {
                for (const auto id : *static_cast<const std::pmr::vector<row_id_t> *>(context))
                    visit(target, id);
            }};
    }

    std::pmr::vector<row_id_t> ids;
    RowSet requested;
};

class ScratchResource final : public std::pmr::memory_resource {
  public:
    explicit ScratchResource(ScratchPad &scratch) noexcept : scratch_{&scratch} {}

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        return scratch_->allocate(bytes, alignment);
    }
    void do_deallocate(void *address, std::size_t bytes, std::size_t) override {
        scratch_->release_last(address, bytes);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == std::addressof(other);
    }

    ScratchPad *scratch_;
};

struct ExpressionNodeInputs {
    ScalarValue<Opaque<CompiledExpressionOwner>> expression;
    WiringPoint columns;
    WiringPoint parameters;
};

// Compiles one bound column expression without materialising a computed column.
ExpressionNodeInputs compile_column_expression(WiringPoint expression, std::size_t first_column);

} // namespace hg::frame::details
