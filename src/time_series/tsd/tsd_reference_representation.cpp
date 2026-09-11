#include "tsd_reference_representation.hpp"

#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

ReferenceRepresentation *TimeSeriesDictionaryRuntimeType::create_reference_representation(
    ReferenceProvider &provider, Reference base_reference, const TimeSeriesType &requested_type,
    allocator &memory) const {
    const auto *dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&requested_type);
    if (dictionary == nullptr || &dictionary->key_type() != &key_runtime_type().type()) {
        return TimeSeriesRuntimeType::create_reference_representation(provider, base_reference, requested_type, memory);
    }

    const auto *requested_reference = dynamic_cast<const TimeSeriesReferenceType *>(&dictionary->value_type());
    if (requested_reference != nullptr && requested_reference->target_type().accepts(item_runtime_type().type())) {
        const auto &provided = static_cast<const TimeSeriesDictionaryType &>(type());
        const auto item_guarantee = strongest(TimeSeriesGuarantee::final, *provided.guarantee().value());
        if (!satisfies(item_guarantee, *requested_reference->guarantee().value())) {
            return TimeSeriesRuntimeType::create_reference_representation(provider, base_reference, requested_type,
                                                                          memory);
        }

        const auto &item_type = time_series_reference_type(item_runtime_type().type(), item_guarantee);
        const auto &representation_type =
            time_series_dictionary_type(provided.key_type(), item_type, *provided.guarantee().value());
        const auto &item = resolve_time_series_runtime_type(item_type);
        return make_tsd_item_reference_representation(provider, base_reference, representation_type, *this, item, memory);
    }

    const auto *base_item_reference_type = dynamic_cast<const TimeSeriesReferenceType *>(&item_runtime_type().type());
    if (base_item_reference_type != nullptr &&
        dictionary->value_type().accepts(base_item_reference_type->target_type())) {
        const auto &item = resolve_time_series_runtime_type(dictionary->value_type());
        return make_dereferenced_tsd_representation(provider, base_reference, *dictionary, *this, item, memory);
    }

    return TimeSeriesRuntimeType::create_reference_representation(provider, base_reference, requested_type, memory);
}

} // namespace bg::details
