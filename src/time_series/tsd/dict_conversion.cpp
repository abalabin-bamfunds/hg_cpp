#include <hg/time_series/tsd/details/dict_conversion.hpp>

#include <hg/graph/wiring_argument.hpp>
#include <hg/node.hpp>
#include <hg/scalar/dict/scalar.hpp>
#include <hg/scalar/dict/scalar_type.hpp>
#include <hg/time_series/tsd/tsd_input.hpp>
#include <hg/time_series/tsd/tsd_output.hpp>
#include <hg/type/time_series_dict_type.hpp>
#include <hg/type/time_series_value_type.hpp>

#include <array>

namespace hg::details {
namespace {

using KEY = ScalarVariable<"DICT_TO_TSD_KEY", Hashable>;
using VALUE = ScalarVariable<"DICT_TO_TSD_VALUE">;
inline constexpr auto INPUT_G = guarantee_variable<"DICT_TO_TSD_INPUT_G">;
inline constexpr auto ITEM_G = guarantee_variable<"DICT_TO_TSD_ITEM_G">;
inline constexpr auto OUTPUT_G = guarantee_variable<"DICT_TO_TSD_OUTPUT_G">;

struct DictToTsdNode {
    In<TS<Dict<KEY, VALUE>, INPUT_G>> value;
    Out<TSD<KEY, TS<VALUE, ITEM_G>, OUTPUT_G>> output;

    void evaluate() const {
        const auto values = value.value_view();
        if (!output.valid()) {
            output.clear();
        }

        output.for_each_key([&](const auto &existing) {
            if (!values.contains(existing)) {
                output.erase(existing);
            }
        });
        values.for_each([&](const auto &key, const auto &item) { output[key].set_value(item); });
    }
};

} // namespace

WiringPoint wire_dict_to_tsd(WiringPoint input, const TimeSeriesDictionaryType &output_type) {
    const auto &source = type_cast<TimeSeriesValueType>(input.type().dereferenced_type());
    const auto &dictionary = type_cast<DictScalarType>(source.value_type());
    const auto &output_item = type_cast<TimeSeriesValueType>(output_type.value_type());

    auto type_map = TypeMap{};
    type_map.set(scalar_variable_instance<KEY>(), &dictionary.key_type());
    type_map.set(scalar_variable_instance<VALUE>(), &dictionary.mapped_type());
    type_map.set(guarantee_variable_instance<ITEM_G>(), *output_item.guarantee().value());
    type_map.set(guarantee_variable_instance<OUTPUT_G>(), *output_type.guarantee().value());

    static const auto converter = node<DictToTsdNode>();
    const auto arguments = std::array{WiringArgument{input}};
    auto &wiring = WiringContext::current().wiring();
    return *wiring.invoke(converter, arguments, &type_map);
}

} // namespace hg::details
