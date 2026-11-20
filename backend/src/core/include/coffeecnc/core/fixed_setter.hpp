#ifndef COFFEECNC_CORE_FIXED_SETTER_HPP
#define COFFEECNC_CORE_FIXED_SETTER_HPP

#include <array>
#include <cstdint>
#include <optional>

namespace coffeecnc {

struct FixedSetterLimits {
  double travel_mm;
  double feed_mm_min;
  int reference_tool{98};
  double xy_tolerance_mm{};
  friend auto operator==(const FixedSetterLimits&, const FixedSetterLimits&) -> bool = default;
};

enum class SetterAction : std::uint8_t { status, capture, calibrate, measure, accept, dismiss };

struct FixedSetterRequest {
  SetterAction action;
  int tool{};
  int wcs{};
  double height{};
  double capture_z{};
  std::array<double, 3> start{};
  std::int64_t review{};
};

struct SetterEpoch {
  std::uint64_t page;
  std::uint64_t datum;
  std::uint64_t tool;
  std::uint64_t stop;
  friend auto operator==(const SetterEpoch&, const SetterEpoch&) -> bool = default;
};

struct SetterReview {
  std::int64_t id;
  bool calibration;
  int tool;
  std::array<double, 3> contact;
  double value; // calibration plane or measured tool length in mm
  double previous_length;
  double stopping_distance; // downward travel from the latch to settled machine Z in mm
};

struct SetterState {
  bool reference_captured{};
  std::optional<std::array<double, 3>> calibration; // XY and effective trigger plane Z in mm
  std::optional<SetterReview> review;
};

} // namespace coffeecnc

#endif
