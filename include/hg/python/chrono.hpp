#pragma once

#include <hg/evaluation_time.hpp>

#include <nanobind/stl/chrono.h>

#include <chrono>
#include <cstdint>

namespace nanobind::detail {

// HG timestamps use timezone-naive UTC calendar fields and exact microseconds.
template <> class type_caster<hg::EvaluationTime> {
  public:
    NB_TYPE_CASTER(hg::EvaluationTime, const_name("datetime.datetime"))

    bool from_python(handle source, std::uint32_t, cleanup_list *cleanup) noexcept {
        if (!source) {
            return false;
        }

        std::int32_t parts[8];
        const auto kind = NB_CALL(datetime_unpack)(
            NB_CTX_C(cleanup), source.ptr(), static_cast<std::uint32_t>(datetime_kind::datetime), parts);
        if (kind <= 0) {
            PyErr_Clear();
            return false;
        }

        auto *offset = PyObject_CallMethod(source.ptr(), "utcoffset", nullptr);
        if (offset == nullptr) {
            PyErr_Clear();
            return false;
        }
        const auto naive = offset == Py_None;
        Py_DECREF(offset);
        if (!naive) {
            return false;
        }

        using namespace std::chrono;
        if (parts[0] < 1 || parts[0] > 9999 || parts[1] < 1 || parts[1] > 12 ||
            parts[2] < 1 || parts[2] > 31) {
            return false;
        }
        const auto date = year{parts[0]} / month{static_cast<unsigned int>(parts[1])} /
                          day{static_cast<unsigned int>(parts[2])};
        if (!date.ok() || parts[3] < 0 || parts[3] >= 24 || parts[4] < 0 || parts[4] >= 60 ||
            parts[5] < 0 || parts[5] >= 60 || parts[6] < 0 || parts[6] >= 1'000'000) {
            return false;
        }

        value = hg::EvaluationTime{sys_days{date}} + hours{parts[3]} + minutes{parts[4]} + seconds{parts[5]} +
                microseconds{parts[6]};
        return true;
    }

    static handle from_cpp(hg::EvaluationTime source, rv_policy, cleanup_list *cleanup) noexcept {
        using namespace std::chrono;
        constexpr auto first = sys_days{year{1} / January / 1};
        constexpr auto last = sys_days{year{10000} / January / 1};
        if (source < first || source >= last) {
            PyErr_SetString(PyExc_OverflowError, "hg.EvaluationTime is outside the datetime.datetime range");
            return {};
        }

        const auto date = floor<days>(source);
        const auto calendar = year_month_day{date};
        const auto time = hh_mm_ss{source - date};
        return pack_datetime(static_cast<int>(calendar.year()), static_cast<unsigned int>(calendar.month()),
                             static_cast<unsigned int>(calendar.day()), static_cast<int>(time.hours().count()),
                             static_cast<int>(time.minutes().count()), static_cast<int>(time.seconds().count()),
                             static_cast<int>(time.subseconds().count()), cleanup);
    }
};

} // namespace nanobind::detail
