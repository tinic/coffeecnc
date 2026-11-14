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

#include <inifile.hh>

namespace coffeecnc {
namespace {

constexpr double millimetres_per_inch = 25.4;
constexpr std::string_view default_nml_file = "/usr/share/linuxcnc/linuxcnc.nml";

auto upper(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(),
                         [](unsigned char c) -> char { return static_cast<char>(std::toupper(c)); });
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

} // namespace

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
    return std::unexpected(std::format(
        "[TRAJ] COORDINATES is \"{}\": only an X Y Z mill is supported.", coordinates));
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
      return std::unexpected(
          std::format("[{}] needs MIN_LIMIT below MAX_LIMIT.", sections[i]));
    travel[i] = {*low * scale, *high * scale};
  }

  auto jog = ini.real("DISPLAY", "MAX_LINEAR_VELOCITY");
  if (!jog)
    jog = ini.real("TRAJ", "MAX_LINEAR_VELOCITY");
  if (!jog || *jog <= 0)
    return std::unexpected(
        std::string("[DISPLAY] or [TRAJ] MAX_LINEAR_VELOCITY is required for jogging."));
  auto rpm = ini.real("SPINDLE_0", "MAX_FORWARD_VELOCITY");
  if (!rpm)
    rpm = ini.real("DISPLAY", "MAX_SPINDLE_0_SPEED");
  if (!rpm || *rpm <= 0)
    return std::unexpected(std::string("[SPINDLE_0] MAX_FORWARD_VELOCITY is required."));
  const double feed = ini.real("DISPLAY", "MAX_FEED_OVERRIDE").value_or(1.0);
  const double spindle_min = ini.real("DISPLAY", "MIN_SPINDLE_OVERRIDE").value_or(0.5);
  const double spindle_max = ini.real("DISPLAY", "MAX_SPINDLE_OVERRIDE").value_or(1.0);
  if (feed < 1 || spindle_min < 0 || spindle_min > 1 || spindle_max < 1)
    return std::unexpected(std::string(
        "[DISPLAY] MAX_FEED_OVERRIDE and MAX_SPINDLE_OVERRIDE must be at least 1, "
        "MIN_SPINDLE_OVERRIDE between 0 and 1."));

  const auto path = [&](const char* section, const char* key) -> std::string {
    const auto value = ini.text(section, key);
    return value && !value->empty() ? absolute(directory, *value) : std::string();
  };
  constexpr double seconds_per_minute = 60;
  return MachineConfig{
      .ini_file = file.string(),
      .directory = directory.string(),
      .machine_name = ini.text("EMC", "MACHINE").value_or(""),
      .nml_file = ini.text("EMC", "NML_FILE").value_or(std::string(default_nml_file)),
      .machine_units = scale == 1 ? LengthUnit::mm : LengthUnit::inch,
      .millimetres_per_unit = scale,
      .travel = travel,
      .limits = Limits{*jog * scale * seconds_per_minute, feed, spindle_min, spindle_max, *rpm},
      .tool_table = path("EMCIO", "TOOL_TABLE"),
      .postgui_halfile = path("HAL", "POSTGUI_HALFILE"),
      .program_prefix = path("DISPLAY", "PROGRAM_PREFIX"),
  };
}

} // namespace coffeecnc
