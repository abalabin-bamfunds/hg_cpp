#include "graph_run_output.hpp"

#include <bg/python/scalar_conversion.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include "delta.hpp"

#include <nanobind/stl/chrono.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nb = nanobind;

namespace bg::python::details {

nb::tuple GraphRunOutputs::to_python(std::size_t minimum_size) const {
    auto output = nb::list{};
    for (auto index = std::size_t{}; index < results_.event_count(); ++index) {
        results_.visit_event(index, [&](EvaluationTime time, const TimeSeriesType &type, bool valid,
                                        util::buffer_reader &reader) {
            auto value = valid ? delta(type, true, reader) : nb::none();
            output.append(nb::make_tuple(time, std::move(value)));
        });
    }
    return nb::make_tuple(results_.start_time(), output, std::max(results_.size(), minimum_size));
}

nb::object GraphRunOutputs::scalar(util::buffer_reader &reader, const ScalarType &type) const {
    auto value = nb::object{};
    const auto read = bg::details::TimeSeriesTickScalarRead::visit(reader, type, [&](Scalar<> item) {
        value = scalar_conversions().to_python(item);
    });
    if (!read) {
        throw std::invalid_argument{"run_graph result scalar has no registered tick reader"};
    }
    return value;
}

nb::object GraphRunOutputs::delta(const TimeSeriesType &type, bool valid, util::buffer_reader &reader) const {
    if (!valid) {
        return nb::borrow<nb::object>(invalid_sentinel());
    }

    return type.visit(
        [&](const TimeSeriesValueType *value_type) {
            return scalar(reader, value_type->value_type());
        },
        [&](const TimeSeriesListType *list_type) {
            auto result = nb::dict{};
            for (;;) {
                const auto header = bg::details::TickHeader::read(reader);
                if (header.operation == bg::details::TickOperation::end) {
                    break;
                }
                if (header.operation != bg::details::TickOperation::tick) {
                    throw std::invalid_argument{"invalid TSL tick entry"};
                }

                const auto item_id = reader.read_raw<item_id_t>();
                result[nb::int_(item_id)] = delta(list_type->item_type(item_id), header.valid, reader);
            }
            return nb::object{std::move(result)};
        },
        [&](const TimeSeriesBundleType *bundle_type) {
            auto result = nb::dict{};
            for (;;) {
                const auto header = bg::details::TickHeader::read(reader);
                if (header.operation == bg::details::TickOperation::end) {
                    break;
                }
                if (header.operation != bg::details::TickOperation::tick) {
                    throw std::invalid_argument{"invalid TSB tick entry"};
                }

                const auto item_id = reader.read_raw<item_id_t>();
                const auto name = bundle_type->item_name(item_id).text();
                const auto key = nb::str{name.data(), name.size()};
                result[key] = delta(bundle_type->item_type(item_id), header.valid, reader);
            }
            return nb::object{std::move(result)};
        },
        [&](const TimeSeriesSetType *set_type) {
            auto added = nb::set{};
            auto removed = nb::set{};
            for (;;) {
                const auto header = bg::details::TickHeader::read(reader);
                if (header.operation == bg::details::TickOperation::end) {
                    break;
                }
                if (header.operation != bg::details::TickOperation::add &&
                    header.operation != bg::details::TickOperation::remove) {
                    throw std::invalid_argument{"invalid TSS tick entry"};
                }

                auto &values = header.operation == bg::details::TickOperation::add ? added : removed;
                values.add(scalar(reader, set_type->item_type()));
            }
            return make_set_delta(added, removed);
        },
        [&](const TimeSeriesDictionaryType *dictionary_type) {
            auto result = nb::dict{};
            for (;;) {
                const auto header = bg::details::TickHeader::read(reader);
                if (header.operation == bg::details::TickOperation::end) {
                    break;
                }
                if (header.operation != bg::details::TickOperation::add &&
                    header.operation != bg::details::TickOperation::tick &&
                    header.operation != bg::details::TickOperation::remove) {
                    throw std::invalid_argument{"invalid TSD tick entry"};
                }

                auto key = scalar(reader, dictionary_type->key_type());
                if (header.operation == bg::details::TickOperation::remove) {
                    result[key] = nb::borrow<nb::object>(remove_sentinel());
                } else {
                    result[key] = delta(dictionary_type->value_type(), header.valid, reader);
                }
            }
            return nb::object{std::move(result)};
        },
        [&]() -> nb::object {
            throw std::invalid_argument{"run_graph Python result conversion does not support this time-series type"};
        }).value();
}

} // namespace bg::python::details
