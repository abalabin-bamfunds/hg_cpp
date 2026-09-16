#include "tsd_reference_representation.hpp"

#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <stdexcept>

namespace bg::details {
namespace {

bool representation_required(const TimeSeriesRuntimeType &provided, const TimeSeriesRuntimeType &requested) noexcept {
    const auto &provided_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(provided);
    const auto &requested_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(requested);
    if (provided_tsd.item_runtime_type().is_reference() != requested_tsd.item_runtime_type().is_reference()) {
        return true;
    }
    return ReferenceProvider::representation_required(provided_tsd.item_runtime_type(),
                                                      requested_tsd.item_runtime_type());
}

const TimeSeriesRuntimeType &representation_runtime_type(const TimeSeriesRuntimeType &provided,
                                                         const TimeSeriesRuntimeType &requested) {
    const auto &provided_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(provided);
    const auto &requested_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(requested);
    if (requested_tsd.item_runtime_type().is_reference() && !provided_tsd.item_runtime_type().is_reference()) {
        const auto &provided_type = static_cast<const TimeSeriesDictionaryType &>(provided_tsd.type());
        const auto item_guarantee = strongest(TimeSeriesGuarantee::final, *provided_type.guarantee().value());
        const auto &item_type = time_series_reference_type(provided_tsd.item_runtime_type().type(), item_guarantee);
        const auto &representation_type =
            time_series_dictionary_type(provided_type.key_type(), item_type, *provided_type.guarantee().value());
        const auto &item_runtime_type = resolve_time_series_runtime_type(item_type);
        return time_series_dictionary_runtime_type(representation_type, provided_tsd.key_runtime_type(),
                                                   item_runtime_type);
    }
    if (provided_tsd.item_runtime_type().is_reference() && !requested_tsd.item_runtime_type().is_reference()) {
        return requested_tsd;
    }

    throw std::invalid_argument{"the requested TSD representation is not implemented"};
}

ReferenceRepresentation *create_representation(ReferenceProvider &provider, Reference base_reference,
                                               const TimeSeriesRuntimeType &provided,
                                               const TimeSeriesRuntimeType &requested, allocator &memory) {
    const auto &provided_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(provided);
    const auto &requested_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(requested);
    const auto &requested_type = static_cast<const TimeSeriesDictionaryType &>(requested_tsd.type());
    if (requested_tsd.item_runtime_type().is_reference() && !provided_tsd.item_runtime_type().is_reference()) {
        return make_tsd_item_reference_representation(provider, base_reference, requested_type, provided_tsd,
                                                      requested_tsd.item_runtime_type(), memory);
    }
    if (provided_tsd.item_runtime_type().is_reference() && !requested_tsd.item_runtime_type().is_reference()) {
        return make_dereferenced_tsd_representation(provider, base_reference, requested_type, provided_tsd,
                                                    requested_tsd.item_runtime_type(), memory);
    }

    throw std::invalid_argument{"the requested TSD representation is not implemented"};
}

const ReferenceRepresentationFactory factory{&representation_required, &representation_runtime_type,
                                             &create_representation};

} // namespace

const ReferenceRepresentationFactory *
TimeSeriesDictionaryRuntimeType::reference_representation_factory() const noexcept {
    return &factory;
}

} // namespace bg::details
