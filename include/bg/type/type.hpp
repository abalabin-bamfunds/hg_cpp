#pragma once

#include <ddv/visitable.h>

namespace bg {

class DictScalarType;
class ListScalarType;
class ScalarType;
class SetScalarType;
class TimeSeriesBundleType;
class TimeSeriesDictionaryType;
class TimeSeriesListType;
class TimeSeriesReferenceType;
class TimeSeriesSetType;
class TimeSeriesType;
class TimeSeriesValueType;
class Type;
class TypeMap;

using TypeVisitor = ddv::mux<Type, ScalarType, ListScalarType, SetScalarType, DictScalarType, TimeSeriesType,
                             TimeSeriesValueType, TimeSeriesReferenceType, TimeSeriesListType, TimeSeriesBundleType,
                             TimeSeriesSetType, TimeSeriesDictionaryType>;

class Type : public ddv::visitable<Type, TypeVisitor> {
  public:
    virtual ~Type();

    VISITOR_SUPPORT()

    virtual bool is_resolved() const noexcept = 0;
    virtual bool is_scalar() const noexcept { return false; }
    virtual bool is_time_series() const noexcept { return false; }
    [[nodiscard]] bool match(const Type &supplied, TypeMap &type_map) const;
    [[nodiscard]] virtual const Type &resolve(const TypeMap &type_map) const = 0;

  protected:
    [[nodiscard]] virtual bool do_match(const Type &supplied, TypeMap &) const { return this == &supplied; }
};

} // namespace bg
