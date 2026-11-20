#include <coffeecnc/linuxcnc/machine_config.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include <inifile.hh>

namespace coffeecnc {
namespace {

constexpr double millimetres_per_inch = 25.4;
constexpr std::string_view default_nml_file = "/usr/share/linuxcnc/linuxcnc.nml";

auto upper(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char c) -> char {
    return static_cast<char>(std::toupper(c));
  });
  return text;
}

auto without_spaces(std::string text) -> std::string {
  std::erase_if(text, [](unsigned char c) -> bool { return std::isspace(c) != 0; });
  return text;
}

// The whole text as one finite number.
auto number(std::string_view text) -> std::optional<double> {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    text.remove_suffix(1);
  double value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
    return {};
  return value;
}

class Ini {
public:
  explicit Ini(const std::string& file) { open_ = file_.Open(file.c_str()); }
  [[nodiscard]] auto open() const -> bool { return open_; }
  auto text(const char* section, const char* key) -> std::optional<std::string> {
    const char* value = file_.Find(key, section);
    if (value == nullptr)
      return {};
    return std::string(value);
  }
  auto real(const char* section, const char* key) -> std::optional<double> {
    const auto value = text(section, key);
    return value ? number(*value) : std::nullopt;
  }

private:
  IniFile file_;
  bool open_{};
};

auto absolute(const std::filesystem::path& directory, const std::string& path) -> std::string {
  const std::filesystem::path given{path};
  return (given.is_absolute() ? given : directory / given).lexically_normal().string();
}

// A yes/no setting of the [COFFEECNC] section. Absent is no.
auto flag(Ini& ini, const char* key) -> std::expected<bool, std::string> {
  const auto given = ini.text("COFFEECNC", key);
  if (!given)
    return false;
  const auto word = upper(without_spaces(*given));
  if (word == "YES" || word == "TRUE" || word == "ON" || word == "1")
    return true;
  if (word == "NO" || word == "FALSE" || word == "OFF" || word == "0")
    return false;
  return std::unexpected(std::format("[COFFEECNC] {} is \"{}\": use yes or no.", key, *given));
}

// [TRAJ] NO_FORCE_HOMING as LinuxCNC's task reads it (sscanf %d): the leading integer, above
// zero for "MDI and programs run unhomed". Task falls back to 0 for text without one; here that
// is an error, so that the panel never shows a rule the operator did not configure.
auto homing_required(Ini& ini) -> std::expected<bool, std::string> {
  const auto given = ini.text("TRAJ", "NO_FORCE_HOMING");
  if (!given)
    return true;
  std::string_view text{*given};
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    text.remove_prefix(1);
  if (!text.empty() && text.front() == '+')
    text.remove_prefix(1);
  int value{};
  if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{})
    return std::unexpected(std::format("[TRAJ] NO_FORCE_HOMING is \"{}\": use 0 or 1.", *given));
  return value <= 0;
}

// One [DISPLAY] INCREMENTS entry, without spaces and in lower case, as millimetres.
auto increment(std::string_view entry, double millimetres_per_unit) -> std::optional<double> {
  // `inch` before `in`: the longer suffix first.
  constexpr std::array<std::pair<std::string_view, double>, 6> suffixes{{
      {"inch", millimetres_per_inch},
      {"mil", millimetres_per_inch / 1000},
      {"mm", 1},
      {"cm", 10},
      {"um", 0.001},
      {"in", millimetres_per_inch},
  }};
  double scale = millimetres_per_unit;
  for (const auto& [suffix, millimetres] : suffixes) {
    if (entry.ends_with(suffix)) {
      scale = millimetres;
      entry.remove_suffix(suffix.size());
      break;
    }
  }
  std::optional<double> value;
  if (const auto slash = entry.find('/'); slash != std::string_view::npos) {
    const auto numerator = number(entry.substr(0, slash));
    const auto denominator = number(entry.substr(slash + 1));
    if (numerator && denominator && *denominator != 0)
      value = *numerator / *denominator;
  } else {
    value = number(entry);
  }
  if (!value || !std::isfinite(*value * scale) || *value * scale <= 0)
    return {};
  return *value * scale;
}

} // namespace

auto parse_jog_increments(std::string_view text, double millimetres_per_unit)
    -> std::expected<std::vector<double>, std::string> {
  // As AXIS reads the list: a comma anywhere makes commas the separators, and `1 mm` one entry.
  const std::string_view separators = text.find(',') != std::string_view::npos ? "," : " \t\r\n";
  std::vector<double> increments;
  while (!text.empty()) {
    const auto end = std::min(text.find_first_of(separators), text.size());
    const auto entry = std::string(text.substr(0, end));
    text.remove_prefix(std::min(end + 1, text.size()));
    auto word = without_spaces(entry);
    std::ranges::transform(word, word.begin(), [](unsigned char c) -> char {
      return static_cast<char>(std::tolower(c));
    });
    if (word.empty() || word == "continuous")
      continue;
    const auto value = increment(word, millimetres_per_unit);
    if (!value)
      return std::unexpected(std::format(
          "[DISPLAY] INCREMENTS entry \"{}\" is not a length above zero.", without_spaces(entry)));
    increments.push_back(*value);
  }
  return increments;
}

auto load_machine_config(const std::string& ini_file) -> std::expected<MachineConfig, std::string> {
  std::error_code error;
  const auto file = std::filesystem::absolute(ini_file, error);
  if (error)
    return std::unexpected(std::format("Cannot resolve INI file {}.", ini_file));
  Ini ini{file.string()};
  if (!ini.open())
    return std::unexpected(std::format("Cannot read INI file {}.", file.string()));
  const auto directory = file.parent_path();

  const auto units = upper(without_spaces(ini.text("TRAJ", "LINEAR_UNITS").value_or("")));
  double scale{};
  if (units == "MM" || units == "METRIC")
    scale = 1;
  else if (units == "IN" || units == "INCH" || units == "IMPERIAL")
    scale = millimetres_per_inch;
  else
    return std::unexpected(
        std::format("[TRAJ] LINEAR_UNITS is \"{}\": only mm and inch machines are supported.",
                    ini.text("TRAJ", "LINEAR_UNITS").value_or("")));

  const auto coordinates = upper(without_spaces(ini.text("TRAJ", "COORDINATES").value_or("")));
  if (coordinates != "XYZ")
    return std::unexpected(
        std::format("[TRAJ] COORDINATES is \"{}\": only an X Y Z mill is supported.", coordinates));
  // Joint n must be axis n. trivkins is that only with its default or the same coordinates and
  // without kinstype=both, which makes the kinematics non-identity for LinuxCNC.
  const auto kinematics = upper(ini.text("KINS", "KINEMATICS").value_or(""));
  const auto words = without_spaces(kinematics);
  if (words != "TRIVKINS" && words != "TRIVKINSCOORDINATES=XYZ")
    return std::unexpected(
        std::format("[KINS] KINEMATICS is \"{}\": only trivkins with joints 0, 1, 2 as X, Y, Z is "
                    "supported.",
                    ini.text("KINS", "KINEMATICS").value_or("")));
  if (ini.real("KINS", "JOINTS") != 3.0)
    return std::unexpected(std::string("[KINS] JOINTS must be 3."));

  std::array<Travel, 3> travel{};
  constexpr std::array<const char*, 3> sections{"AXIS_X", "AXIS_Y", "AXIS_Z"};
  for (std::size_t i = 0; i < travel.size(); ++i) {
    const auto low = ini.real(sections[i], "MIN_LIMIT");
    const auto high = ini.real(sections[i], "MAX_LIMIT");
    if (!low || !high || *low >= *high)
      return std::unexpected(std::format("[{}] needs MIN_LIMIT below MAX_LIMIT.", sections[i]));
    travel[i] = {*low * scale, *high * scale};
  }

  constexpr double seconds_per_minute = 60;
  auto jog = ini.real("DISPLAY", "MAX_LINEAR_VELOCITY");
  if (!jog)
    jog = ini.real("TRAJ", "MAX_LINEAR_VELOCITY");
  if (!jog || *jog <= 0)
    return std::unexpected(
        std::string("[DISPLAY] or [TRAJ] MAX_LINEAR_VELOCITY is required for jogging."));
  const double jog_ceiling = *jog * scale * seconds_per_minute;
  std::array<double, 3> axis_jog{};
  for (std::size_t i = 0; i < axis_jog.size(); ++i) {
    const auto velocity = ini.real(sections[i], "MAX_VELOCITY");
    if (!velocity || *velocity <= 0)
      return std::unexpected(
          std::format("[{}] MAX_VELOCITY is required for jogging.", sections[i]));
    axis_jog[i] = std::min(*velocity * scale * seconds_per_minute, jog_ceiling);
  }
  auto increments =
      parse_jog_increments(ini.text("DISPLAY", "INCREMENTS").value_or(std::string()), scale);
  if (!increments)
    return std::unexpected(std::move(increments.error()));

  // CoffeeCNC's own setting wins and is never skipped when it is wrong. Another machine's custom
  // keys (a VFD component's, for one) are not read: their meaning is that machine's.
  std::optional<double> rpm;
  if (const auto own = ini.text("COFFEECNC", "MAX_SPINDLE_RPM")) {
    rpm = number(*own);
    if (!rpm || *rpm <= 0)
      return std::unexpected(std::format(
          "[COFFEECNC] MAX_SPINDLE_RPM is \"{}\": a spindle speed above zero is required.", *own));
  } else {
    rpm = ini.real("SPINDLE_0", "MAX_FORWARD_VELOCITY");
    if (!rpm)
      rpm = ini.real("DISPLAY", "MAX_SPINDLE_0_SPEED");
    if (!rpm)
      return std::unexpected(
          std::string("The highest spindle speed is not configured: set [COFFEECNC] "
                      "MAX_SPINDLE_RPM, [SPINDLE_0] MAX_FORWARD_VELOCITY or [DISPLAY] "
                      "MAX_SPINDLE_0_SPEED."));
    if (*rpm <= 0)
      return std::unexpected(
          std::string("[SPINDLE_0] MAX_FORWARD_VELOCITY or [DISPLAY] MAX_SPINDLE_0_SPEED must be "
                      "above zero."));
  }
  const auto homing = homing_required(ini);
  if (!homing)
    return std::unexpected(homing.error());
  constexpr std::array<const char*, 3> joints{"JOINT_0", "JOINT_1", "JOINT_2"};
  const bool home_all = std::ranges::all_of(joints, [&](const char* joint) -> bool {
    return ini.text(joint, "HOME_SEQUENCE").has_value();
  });
  HalSignals signals;
  for (const auto& [key, wired] : {std::pair{"SPINDLE_RPM_SIGNAL", &signals.spindle_rpm},
                                   std::pair{"SPINDLE_AT_SPEED_SIGNAL", &signals.spindle_at_speed},
                                   std::pair{"PROBE_SIGNAL", &signals.probe}}) {
    const auto value = flag(ini, key);
    if (!value)
      return std::unexpected(value.error());
    *wired = *value;
  }
  const double feed = ini.real("DISPLAY", "MAX_FEED_OVERRIDE").value_or(1.0);
  const double spindle_min = ini.real("DISPLAY", "MIN_SPINDLE_OVERRIDE").value_or(0.5);
  const double spindle_max = ini.real("DISPLAY", "MAX_SPINDLE_OVERRIDE").value_or(1.0);
  if (feed < 1 || spindle_min < 0 || spindle_min > 1 || spindle_max < 1)
    return std::unexpected(
        std::string("[DISPLAY] MAX_FEED_OVERRIDE and MAX_SPINDLE_OVERRIDE must be at least 1, "
                    "MIN_SPINDLE_OVERRIDE between 0 and 1."));

  std::optional<FixedSetterLimits> setter;
  const auto setter_enabled = flag(ini, "FIXED_SETTER");
  if (!setter_enabled)
    return std::unexpected(setter_enabled.error());
  if (*setter_enabled) {
    const auto distance = ini.real("COFFEECNC", "SETTER_MAX_TRAVEL_MM");
    const auto speed = ini.real("COFFEECNC", "SETTER_MAX_FEED_MM_MIN");
    const auto xy = ini.real("COFFEECNC", "SETTER_XY_TOLERANCE_MM");
    const auto reference = ini.text("COFFEECNC", "SETTER_REFERENCE_TOOL");
    const auto tool = reference ? number(*reference) : std::optional<double>{98};
    if (!signals.probe || !signals.spindle_rpm || !distance || *distance <= 0 || !speed ||
        *speed <= 0 || *speed > axis_jog[2] || !xy || *xy <= 0 || !tool || *tool < 1 ||
        *tool > 99999 || std::floor(*tool) != *tool)
      return std::unexpected(std::string(
          "[COFFEECNC] FIXED_SETTER requires probe and spindle RPM signals, a positive "
          "SETTER_MAX_TRAVEL_MM, SETTER_XY_TOLERANCE_MM, SETTER_MAX_FEED_MM_MIN within the Z "
          "speed limit, and a valid SETTER_REFERENCE_TOOL."));
    setter = FixedSetterLimits{*distance, *speed, static_cast<int>(*tool), *xy};
  }

  const auto path = [&](const char* section, const char* key) -> std::string {
    const auto value = ini.text(section, key);
    return value && !value->empty() ? absolute(directory, *value) : std::string();
  };
  return MachineConfig{
      .ini_file = file.string(),
      .directory = directory.string(),
      .machine_name = ini.text("EMC", "MACHINE").value_or(""),
      // Relative to the directory of the INI file, like every other path in it.
      .nml_file =
          absolute(directory, ini.text("EMC", "NML_FILE").value_or(std::string(default_nml_file))),
      .machine_units = scale == 1 ? LengthUnit::mm : LengthUnit::inch,
      .millimetres_per_unit = scale,
      .axes = {Axis::x, Axis::y, Axis::z},
      .travel = travel,
      .jog_increments = std::move(*increments),
      .limits = Limits{jog_ceiling, axis_jog, feed, spindle_min, spindle_max, *rpm, *homing},
      .home_all_configured = home_all,
      .signals = signals,
      .tool_table = path("EMCIO", "TOOL_TABLE"),
      .parameter_file = path("RS274NGC", "PARAMETER_FILE"),
      .postgui_halfile = path("HAL", "POSTGUI_HALFILE"),
      .program_prefix = path("DISPLAY", "PROGRAM_PREFIX"),
      .machine_id = ini.text("COFFEECNC", "MACHINE_ID"),
      .coolant = std::nullopt,
      .data_directory = ini.text("COFFEECNC", "DATA_DIRECTORY"),
      .fixed_setter = setter,
  };
}

} // namespace coffeecnc
