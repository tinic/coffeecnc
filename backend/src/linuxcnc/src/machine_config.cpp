#include <coffeecnc/linuxcnc/machine_config.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <emcmotcfg.h>
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
  auto text(const char* section, const char* key, int occurrence = 1) -> std::optional<std::string> {
    const char* value = file_.Find(key, section, occurrence);
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

auto positive_settings(Ini& ini, std::initializer_list<std::pair<const char*, double*>> fields)
    -> std::expected<void, std::string> {
  for (const auto& [key, out] : fields) {
    if (const auto text = ini.text("COFFEECNC", key)) {
      const auto value = number(*text);
      if (!value || *value <= 0)
        return std::unexpected(std::format("[COFFEECNC] {} must be above zero in mm.", key));
      *out = *value;
    }
  }
  return {};
}

auto default_machine_id(std::string_view name, std::string_view ini_path)
    -> std::optional<std::string> {
  constexpr std::size_t max_name = 47; // 64 characters, including the hyphen and 16 hex digits.
  std::string id;
  bool separator = false;
  for (const char c : name) {
    if (c == '_' || c == '-' || valid_machine_id(std::string_view(&c, 1))) {
      if (id.empty() && (c == '_' || c == '-'))
        continue;
      if (separator && !id.empty() && id.size() + 1 < max_name)
        id += '-';
      separator = false;
      id += c;
      if (id.size() >= max_name)
        break;
    } else {
      separator = true;
    }
  }
  if (id.empty())
    return std::nullopt;
  // Stable across processes. The path distinguishes configurations with the same display name.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char byte : ini_path) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  return std::format("{}-{:016x}", id, hash);
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

// The joints of XYZABC from [TRAJ] COORDINATES, [KINS] JOINTS and KINEMATICS, as trivkins
// assigns them: joint n is the n-th letter of its coordinates= (default XYZABCUVW). A letter may
// repeat: X Y Y Z is a gantry with joints 1 and 2 on Y.
auto axis_joints(Ini& ini) -> std::expected<std::array<std::vector<int>, 6>, std::string> {
  const auto given = ini.text("TRAJ", "COORDINATES").value_or("");
  const auto coordinates = upper(without_spaces(given));
  constexpr std::string_view letters = "XYZABC";
  if (const auto other = coordinates.find_first_not_of(letters); other != std::string::npos)
    return std::unexpected(std::format(
        "[TRAJ] COORDINATES is \"{}\": axis {} is not supported. Only XYZABC are; a letter "
        "may repeat for a gantry (X Y Y Z).",
        given, coordinates[other]));
  if (coordinates.size() > EMCMOT_MAX_JOINTS)
    return std::unexpected(std::format(
        "[TRAJ] COORDINATES has more than {} joints.", EMCMOT_MAX_JOINTS));
  std::array<std::vector<int>, 6> joints{};
  for (std::size_t joint = 0; joint < coordinates.size(); ++joint)
    joints.at(letters.find(coordinates[joint])).push_back(static_cast<int>(joint));
  if (std::ranges::any_of(joints | std::views::take(3),
                          [](const auto& own) -> bool { return own.empty(); }))
    return std::unexpected(std::format(
        "[TRAJ] COORDINATES is \"{}\": X, Y and Z are all required.", given));
  const auto count = ini.real("KINS", "JOINTS");
  const auto count_text = ini.text("KINS", "JOINTS");
  if (count != static_cast<double>(coordinates.size()))
    return std::unexpected(std::format(
        "[KINS] JOINTS is {}: set it to {}, one joint for each letter of [TRAJ] COORDINATES "
        "\"{}\".",
        count_text ? std::format("\"{}\"", *count_text) : std::string("not set"),
        coordinates.size(), given));

  // trivkins takes coordinates= and kinstype=, each as one word.
  const auto kinematics = ini.text("KINS", "KINEMATICS").value_or("");
  std::vector<std::string> words;
  for (std::size_t at = 0; at < kinematics.size();) {
    const auto begin = kinematics.find_first_not_of(" \t", at);
    if (begin == std::string::npos)
      break;
    const auto end = std::min(kinematics.find_first_of(" \t", begin), kinematics.size());
    words.push_back(kinematics.substr(begin, end - begin));
    at = end;
  }
  if (words.empty() || upper(words.front()) != "TRIVKINS")
    return std::unexpected(std::format(
        "[KINS] KINEMATICS is \"{}\": only trivkins (trivial kinematics) is supported.",
        kinematics));
  std::string own = std::string("XYZABCUVW").substr(0, coordinates.size());
  bool named = false;
  for (const auto& word : words | std::views::drop(1)) {
    const auto equals = word.find('=');
    const auto key = upper(word.substr(0, equals));
    const auto value = equals == std::string::npos ? std::string() : word.substr(equals + 1);
    if (key == "COORDINATES") {
      own = upper(value);
      named = true;
    } else if (key == "KINSTYPE") {
      // As trivkins reads it: by the first letter. With both, LinuxCNC refuses world mode until
      // every joint is homed (seen on 2.9.4); CoffeeCNC jogs joints until then in any case.
      const char type = static_cast<char>(std::toupper(static_cast<unsigned char>(
          value.empty() ? '1' : value.front())));
      if (type == 'F' || type == 'I')
        return std::unexpected(std::format(
            "[KINS] KINEMATICS has kinstype={}, which trivkins reads as {} kinematics: use "
            "kinstype=both or leave kinstype out.",
            value, type == 'F' ? "forward-only" : "inverse-only"));
    } else {
      return std::unexpected(std::format(
          "[KINS] KINEMATICS has \"{}\": trivkins takes only coordinates= and kinstype=.",
          word));
    }
  }
  if (own != coordinates)
    return std::unexpected(
        named ? std::format("[KINS] KINEMATICS has coordinates={} but [TRAJ] COORDINATES is "
                            "\"{}\": use the same letters in the same order.",
                            own, given)
              : std::format("[KINS] KINEMATICS has no coordinates=, so trivkins makes the joints "
                            "{}: add coordinates={} to match [TRAJ] COORDINATES.",
                            own, coordinates));
  return joints;
}

} // namespace

auto inspect_machine_ini(const std::string& ini_file)
    -> std::expected<MachineIniInspection, std::string> {
  std::error_code error;
  const auto file = std::filesystem::absolute(ini_file, error);
  if (error)
    return std::unexpected(std::format("Cannot resolve INI file {}.", ini_file));
  if (!std::filesystem::is_regular_file(file, error))
    return std::unexpected(std::format("Cannot read INI file {}.", file.string()));
  Ini ini{file.string()};
  if (!ini.open())
    return std::unexpected(std::format("Cannot read INI file {}.", file.string()));
  MachineIniInspection result{.path = file.string(),
                              .display = ini.text("DISPLAY", "DISPLAY"),
                              .postgui = ini.text("HAL", "POSTGUI_HALFILE"),
                              .halfiles = {}};
  for (std::size_t i = 0; i < coffeecnc_signal_keys.size(); ++i)
    result.signal_configured[i] = ini.text("COFFEECNC", coffeecnc_signal_keys[i]).has_value();
  for (int i = 1; const auto name = ini.text("HAL", "HALFILE", i); ++i)
    result.halfiles.push_back(*name);
  return result;
}

auto linuxcnc_switch(std::string_view value) -> bool {
  if (value.starts_with('+') && value.size() > 1 && value[1] != '-')
    value.remove_prefix(1);
  int number{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  return !value.empty() && error == std::errc{} && end == value.data() + value.size() &&
         number != 0;
}

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

  auto joints = axis_joints(ini);
  if (!joints)
    return std::unexpected(std::move(joints.error()));

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
  // LinuxCNC reads HOME_SEQUENCE as an integer; anything else is no sequence.
  std::size_t joint_count = 0;
  for (const auto& own : *joints)
    joint_count += own.size();
  std::vector<std::optional<int>> sequences;
  for (std::size_t joint = 0; joint < joint_count; ++joint) {
    const auto value = ini.real(std::format("JOINT_{}", joint).c_str(), "HOME_SEQUENCE");
    sequences.push_back(value && std::floor(*value) == *value && std::fabs(*value) < 1000
                            ? std::optional<int>(static_cast<int>(*value))
                            : std::nullopt);
  }
  const bool home_all = std::ranges::all_of(
      sequences, [](const std::optional<int>& sequence) -> bool { return sequence.has_value(); });
  std::array<bool, 3> home_axis{};
  for (std::size_t i = 0; i < home_axis.size(); ++i) {
    const auto& own = joints->at(i);
    const auto first = sequences.at(static_cast<std::size_t>(own.front()));
    home_axis[i] = own.size() == 1 || (first && *first < 0 &&
                                       std::ranges::all_of(own, [&](int joint) -> bool {
                                         return sequences.at(static_cast<std::size_t>(joint)) ==
                                                first;
                                       }));
  }
  HalSignals signals;
  for (const auto& [key, wired] : {std::pair{"SPINDLE_RPM_SIGNAL", &signals.spindle_rpm},
                                   std::pair{"SPINDLE_AT_SPEED_SIGNAL", &signals.spindle_at_speed},
                                   std::pair{"PROBE_SIGNAL", &signals.probe}}) {
    const auto value = flag(ini, key);
    if (!value)
      return std::unexpected(value.error());
    *wired = *value;
  }
  if (const auto marks = flag(ini, "REFERENCE_MARKS"); marks)
    signals.reference_marks = *marks;
  else
    return std::unexpected(marks.error());
  std::optional<PausedZLimits> paused_z;
  if (const auto text = ini.text("COFFEECNC", "PAUSED_Z_MAX_MM")) {
    const auto lift = number(*text);
    const auto ratio = ini.real("AXIS_Z", "OFFSET_AV_RATIO");
    const auto velocity = ini.real("AXIS_Z", "MAX_VELOCITY");
    const auto acceleration = ini.real("AXIS_Z", "MAX_ACCELERATION");
    const double count = 0.001 * scale; // HAL eoffset-scale, in native machine units per count.
    if (!lift || *lift < count || *lift / count > std::numeric_limits<std::int32_t>::max() ||
        *lift >= travel[2].max - travel[2].min || !ratio || *ratio <= 0 || *ratio > 0.9 ||
        !velocity || *velocity <= 0 || !acceleration || *acceleration <= 0 ||
        !signals.spindle_rpm || !signals.spindle_at_speed)
      return std::unexpected("Invalid PAUSED_Z_MAX_MM. Check the Z travel, external offset ratio, "
                             "Z velocity and acceleration, and spindle RPM and at-speed signals.");
    paused_z =
        PausedZLimits{*lift, *ratio * *velocity * scale, *ratio * *acceleration * scale, count};
    signals.paused_z = true;
  }
  const double feed = ini.real("DISPLAY", "MAX_FEED_OVERRIDE").value_or(1.0);
  const double spindle_min = ini.real("DISPLAY", "MIN_SPINDLE_OVERRIDE").value_or(0.5);
  const double spindle_max = ini.real("DISPLAY", "MAX_SPINDLE_OVERRIDE").value_or(1.0);
  if (feed < 1 || spindle_min < 0 || spindle_min > 1 || spindle_max < 1)
    return std::unexpected(
        std::string("[DISPLAY] MAX_FEED_OVERRIDE and MAX_SPINDLE_OVERRIDE must be at least 1, "
                    "MIN_SPINDLE_OVERRIDE between 0 and 1."));

  std::optional<int> probe_tool;
  if (const auto text = ini.text("COFFEECNC", "TOUCH_PROBE_TOOL")) {
    const auto value = number(*text);
    if (!value || *value < 1 || *value > 99999 || std::floor(*value) != *value)
      return std::unexpected(std::string("[COFFEECNC] TOUCH_PROBE_TOOL must be a tool number."));
    probe_tool = static_cast<int>(*value);
  }

  std::optional<FixedSetterLimits> setter;
  const auto setter_enabled = flag(ini, "FIXED_SETTER");
  if (!setter_enabled)
    return std::unexpected(setter_enabled.error());
  if (*setter_enabled) {
    const auto distance = ini.real("COFFEECNC", "SETTER_MAX_TRAVEL_MM");
    const auto search = ini.text("COFFEECNC", "SETTER_SEARCH_FEED_MM_MIN");
    const auto speed = search ? number(*search) : ini.real("COFFEECNC", "SETTER_MAX_FEED_MM_MIN");
    const auto xy = ini.real("COFFEECNC", "SETTER_XY_TOLERANCE_MM");
    const auto reference = ini.text("COFFEECNC", "SETTER_REFERENCE_TOOL");
    const auto tool = reference ? number(*reference) : std::optional<double>{98};
    if (!signals.probe || !signals.spindle_rpm || !distance || *distance <= 0 || !speed ||
        *speed <= 0 || *speed > axis_jog[2] || !xy || *xy <= 0 || !tool || *tool < 1 ||
        *tool > 99999 || std::floor(*tool) != *tool)
      return std::unexpected(std::string(
          "[COFFEECNC] FIXED_SETTER requires probe and spindle RPM signals, a positive "
          "SETTER_MAX_TRAVEL_MM and SETTER_XY_TOLERANCE_MM, SETTER_SEARCH_FEED_MM_MIN "
          "(or SETTER_MAX_FEED_MM_MIN) within the Z speed limit, and a valid SETTER_REFERENCE_TOOL."));
    const auto clearance = ini.text("COFFEECNC", "SETTER_CLEARANCE_MM");
    const auto latch = ini.text("COFFEECNC", "SETTER_LATCH_FEED_MM_MIN");
    const auto backoff = ini.text("COFFEECNC", "SETTER_LATCH_BACKOFF_MM");
    const auto clearance_mm = clearance ? number(*clearance) : std::optional{0.0};
    const auto latch_feed = latch ? number(*latch) : std::optional{0.0};
    const auto latch_backoff = backoff ? number(*backoff) : std::optional{latch ? 1.0 : 0.0};
    const auto material = ini.text("COFFEECNC", "TOUCH_PROBE_MAX_TRAVEL_MM");
    const auto material_travel = material ? number(*material) : std::optional{20.0};
    if (!material_travel || *material_travel <= 0 ||
        (material && *material_travel / *speed * 60 > 180))
      return std::unexpected("[COFFEECNC] TOUCH_PROBE_MAX_TRAVEL_MM must specify a positive "
                             "distance within three minutes at the setter search feed.");
    if (!clearance_mm || (clearance && (*clearance_mm <= 0 || *clearance_mm >= *distance)) ||
        !latch_feed || (latch && (*latch_feed <= 0 || *latch_feed > *speed)) || !latch_backoff ||
        (backoff && !latch) ||
        (latch && (*latch_backoff <= 0 || *latch_backoff >= *distance ||
                   2 * *latch_backoff / *latch_feed * 60 > 180)))
      return std::unexpected("Invalid setter clearance or latch settings. The search must "
                             "reach below the reference latch from clearance.");
    setter =
        FixedSetterLimits{*distance,     *speed,      static_cast<int>(*tool), *xy,
                          *clearance_mm, *latch_feed, *latch_backoff,          *material_travel};
    if (const auto parsed = positive_settings(
            ini, {{"SETTER_CHECK_TOLERANCE_MM", &setter->check_tolerance_mm}}); !parsed)
      return std::unexpected(parsed.error());
    auto& geometry = setter->material_geometry;
    if (const auto parsed = positive_settings(ini,
            {{"TOUCH_PROBE_CORNER_REACH_MM", &geometry.reach_mm},
             {"TOUCH_PROBE_CORNER_MARGIN_MM", &geometry.margin_mm},
             {"TOUCH_PROBE_CLEARANCE_MM", &geometry.clearance_mm},
             {"TOUCH_PROBE_DEPTH_MM", &geometry.depth_mm}}); !parsed)
      return std::unexpected(parsed.error());
  }

  const auto manual_tool_change = flag(ini, "MANUAL_TOOLCHANGE");
  if (!manual_tool_change)
    return std::unexpected(manual_tool_change.error());

  Limits limits{jog_ceiling, axis_jog, feed, spindle_min, spindle_max, *rpm, *homing};
  for (std::size_t i = 0; i < limits.axis_motors.size(); ++i)
    limits.axis_motors[i] = static_cast<int>(joints->at(i).size());

  const auto path = [&](const char* section, const char* key) -> std::string {
    const auto value = ini.text(section, key);
    return value && !value->empty() ? absolute(directory, *value) : std::string();
  };
  const auto name = ini.text("EMC", "MACHINE").value_or("");
  auto machine_id = ini.text("COFFEECNC", "MACHINE_ID");
  if (!machine_id)
    machine_id = default_machine_id(name, file.string());
  MotionTolerances tolerances;
  if (const auto parsed = positive_settings(ini,
          {{"PROBE_OFF_LINE_TOLERANCE_MM", &tolerances.probe_off_line_mm},
           {"PROBE_BACKLASH_TOLERANCE_MM", &tolerances.probe_backlash_mm},
           {"PROBE_POSITION_TOLERANCE_MM", &tolerances.probe_position_mm},
           {"RESTART_POSITION_TOLERANCE_MM", &tolerances.restart_position_mm}}); !parsed)
    return std::unexpected(parsed.error());
  return MachineConfig{
      .ini_file = file.string(),
      .directory = directory.string(),
      .machine_name = name,
      // Relative to the directory of the INI file, like every other path in it.
      .nml_file =
          absolute(directory, ini.text("EMC", "NML_FILE").value_or(std::string(default_nml_file))),
      .machine_units = scale == 1 ? LengthUnit::mm : LengthUnit::inch,
      .millimetres_per_unit = scale,
      .axes = {Axis::x, Axis::y, Axis::z},
      .axis_joints = {std::move(joints->at(0)), std::move(joints->at(1)),
                      std::move(joints->at(2))},
      .rotary_joints = {std::move(joints->at(3)), std::move(joints->at(4)),
                        std::move(joints->at(5))},
      .travel = travel,
      .jog_increments = std::move(*increments),
      .limits = limits,
      .home_all_configured = home_all,
      .home_axis_configured = home_axis,
      .signals = signals,
      .tool_table = path("EMCIO", "TOOL_TABLE"),
      .parameter_file = path("RS274NGC", "PARAMETER_FILE"),
      .postgui_halfile = path("HAL", "POSTGUI_HALFILE"),
      .manual_tool_change = *manual_tool_change,
      .program_prefix = path("DISPLAY", "PROGRAM_PREFIX"),
      .machine_id = std::move(machine_id),
      .coolant = std::nullopt,
      .data_directory = ini.text("COFFEECNC", "DATA_DIRECTORY"),
      .fixed_setter = setter,
      .touch_probe_tool = probe_tool,
      .remember_spindle_tool =
          !linuxcnc_switch(ini.text("EMCIO", "RANDOM_TOOLCHANGER").value_or("")) &&
          ini.text("EMCIO", "DB_PROGRAM").value_or("").empty(),
      .paused_z = paused_z,
      .motion_tolerances = tolerances,
  };
}

auto MachineConfig::touched_off_file() const -> std::string {
  const auto state_dir = runtime_directory.empty() ? std::filesystem::path(tool_table).parent_path()
                                                   : std::filesystem::path(runtime_directory);
  return (state_dir / "coffeecnc-touched-off.txt").string();
}

} // namespace coffeecnc
