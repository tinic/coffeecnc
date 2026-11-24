// The tool models: their rules at every boundary, their wire and file text, the archive on a
// file system that fails at each step, the store on its writer thread, and the real lock and
// file modes in a directory of the host.
//   coffeecnc_tool_model_tests <scratch directory>

#include <coffeecnc/app/tool_model_store.hpp>
#include <coffeecnc/app/tool_models.hpp>
#include <coffeecnc/core/text_file.hpp>

#include "fake_tool_model_files.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

auto has(const std::string& text, std::string_view part) -> bool {
  return text.find(part) != std::string::npos;
}

constexpr auto length_reason = "Dimensions must be between 0.001 and 10,000 mm.";
constexpr auto angle_reason = "Included tip angle must be between 1° and 179°.";
constexpr auto order_reason =
    "Cutting length must fit within stick-out, and stick-out within overall length. Leave some "
    "shank above the cutting section.";
constexpr auto ball_reason = "Cutting length must include the full ball radius.";
constexpr auto tip_reason =
    "The pointed tip does not fit within the cutting length at this diameter and angle.";
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr std::uint64_t last_revision = 9007199254740991;

const std::string machine = "mill-1";
const std::string directory = "/data/mill-1";
const std::string file = directory + "/tool-models.json";
const std::string temporary = directory + "/tool-models.json.tmp";
// What opening does where nothing exists yet: each directory made and then made durable in its
// parent, outermost first; then the lock; then the file.
const std::string opening =
    "make_directory /data, sync_directory /, make_directory /data/mill-1, sync_directory /data, "
    "lock /data/mill-1, read " + file;

// Not a short decimal.
constexpr double odd_length = 35 + 0.1 + 0.2;

// The document of an available snapshot.
auto doc(const ToolModels& models) -> const ToolModelDocument& {
  if (!models.document)
    throw std::runtime_error("The snapshot has no document: " + models.reason);
  return *models.document;
}
auto doc(ToolModels& models) -> ToolModelDocument& {
  if (!models.document)
    throw std::runtime_error("The snapshot has no document: " + models.reason);
  return *models.document;
}

auto at(ToolFamily family) -> std::size_t { return static_cast<std::size_t>(family); }
auto below(double value) -> double { return std::nextafter(value, -infinity); }
auto above(double value) -> double { return std::nextafter(value, infinity); }

auto location(const std::string& id = machine) -> ToolModelLocation {
  return resolve_tool_model_location({id, "/data", {}, {}});
}

auto row(int number) -> ToolBinding {
  return {number, number, 6.35, std::format("tool {}", number)};
}
auto defaults(std::uint64_t base, const ToolDefaults& values, const std::string& id = machine)
    -> ToolModelCommand {
  return SetToolModelDefaults{id, base, values};
}
auto assign(std::uint64_t base, int number, ToolFamily family = ToolFamily::end_mill,
            const ToolOverrides& overrides = {}, const std::string& id = machine)
    -> ToolModelCommand {
  return SetToolModelAssignment{id, base, row(number), family, overrides};
}
auto remove(std::uint64_t base, int number) -> ToolModelCommand {
  return SetToolModelAssignment{machine, base, row(number), std::nullopt, {}};
}
// Defaults that are not the factory's.
auto other_defaults() -> ToolDefaults {
  auto values = factory_tool_defaults();
  values.at(at(ToolFamily::end_mill)).stickout = odd_length;
  values.at(at(ToolFamily::drill)).tip_angle = 135;
  return values;
}
auto saved(std::uint64_t revision, const ToolModelDocument& document,
           const std::string& id = machine) -> std::string {
  const auto text = encode_tool_model_file({id, revision, document});
  require(text.has_value(), "A test document encodes: " + text.error_or(LocalizedReason{}).text);
  return text.value_or("");
}

void rules() {
  const auto factory = factory_tool_defaults();
  // ui/src/tool-model.ts, factoryToolDefaults.
  const ToolDefaults expected{{{10, 22, 10, 72, 35, 90},
                               {10, 20, 10, 72, 35, 90},
                               {10, 60, 10, 100, 75, 118},
                               {10, 15, 10, 60, 30, 90},
                               {10, 12, 10, 60, 30, 90},
                               {50, 12, 20, 70, 40, 90},
                               {10, 20, 10, 70, 35, 90},
                               {.diameter = 4, .shank_diameter = 12, .body_diameter = 28, .body_length = 32,
                                .stylus_length = 20}}};
  require(factory == expected, "The factory defaults are the panel's");
  require(tool_defaults_error(factory).empty(), "The factory defaults are valid");
  constexpr std::array<std::string_view, tool_family_count> names{
      "end-mill", "ball-mill", "drill", "spot-drill", "chamfer", "face-mill", "generic", "touch-probe"};
  for (const auto family : tool_families)
    require(tool_family_name(family) == names.at(at(family)) &&
                tool_family(names.at(at(family))) == family,
            "Every family has its wire name");
  require(!tool_family("End-Mill") && !tool_family("") && !tool_family("endmill"),
          "A family name is matched exactly");

  // The five lengths: 0.001 to 10000, each by itself. The other rules may still object; only
  // the range rule says so in these words.
  const ToolDimensions generic{10, 20, 10, 70, 35, 90};
  const auto error = [](const ToolDimensions& d,
                        ToolFamily family = ToolFamily::generic) -> std::string {
    return tool_dimension_error(family, d).text;
  };
  require(error(generic).empty(), "The base dimensions are valid");
  for (const auto member :
       {&ToolDimensions::diameter, &ToolDimensions::cutting_length,
        &ToolDimensions::shank_diameter, &ToolDimensions::overall_length,
        &ToolDimensions::stickout}) {
    for (const double inside : {0.001, 10000.0}) {
      auto d = generic;
      d.*member = inside;
      require(error(d) != length_reason, std::format("A length of {} is in range", inside));
    }
    for (const double outside : {below(0.001), above(10000.0), 0.0, -1.0, nan, infinity}) {
      auto d = generic;
      d.*member = outside;
      require(error(d) == length_reason, std::format("A length of {} is out of range", outside));
    }
  }
  require(error({0.001, 0.001, 0.001, 0.002, 0.001, 1}).empty() &&
              error({10000, 9999, 10000, 10000, 10000, 179}).empty(),
          "The smallest and the largest valid tool");
  for (const double inside : {1.0, 179.0}) {
    auto d = generic;
    d.tip_angle = inside;
    require(error(d).empty(), "An angle at its bound is valid");
  }
  for (const double outside : {below(1.0), above(179.0), 0.0, 180.0, nan, infinity}) {
    auto d = generic;
    d.tip_angle = outside;
    require(error(d) == angle_reason, std::format("An angle of {} is out of range", outside));
  }
  // Cutting length within stick-out within overall length, and some shank left.
  require(error({10, 35, 10, 70, 35, 90}).empty(), "Cutting length may equal stick-out");
  require(error({10, above(35.0), 10, 70, 35, 90}) == order_reason,
          "Cutting length above stick-out");
  require(error({10, 20, 10, 70, 70, 90}).empty(), "Stick-out may equal overall length");
  require(error({10, 20, 10, 70, above(70.0), 90}) == order_reason,
          "Stick-out above overall length");
  require(error({10, 70, 10, 70, 70, 90}) == order_reason &&
              error({10, below(70.0), 10, 70, 70, 90}).empty(),
          "Cutting length must stay below overall length");
  // A ball's radius.
  require(error({10, 5, 10, 70, 35, 90}, ToolFamily::ball_mill).empty() &&
              error({10, below(5.0), 10, 70, 35, 90}, ToolFamily::ball_mill) == ball_reason &&
              error({10, below(5.0), 10, 70, 35, 90}, ToolFamily::end_mill).empty(),
          "Only a ball mill needs its radius within the cutting length");
  // A cone's height, as the panel computes it.
  for (const auto family : {ToolFamily::drill, ToolFamily::spot_drill, ToolFamily::chamfer}) {
    for (const double angle : {90.0, 118.0, 1.0, 179.0}) {
      const double diameter = angle == 1.0 ? 1 : 10;
      const double height = diameter / 2 / std::tan(angle * std::numbers::pi / 360);
      require(error({diameter, height, 10, 9000, 8000, angle}, family).empty() &&
                  error({diameter, below(height), 10, 9000, 8000, angle}, family) == tip_reason,
              std::format("A {} degree tip of family {} fits exactly its height", angle,
                          tool_family_name(family)));
    }
  }
  require(error({10, 1, 10, 70, 35, 90}, ToolFamily::face_mill).empty() &&
              error({10, 1, 10, 70, 35, 90}, ToolFamily::generic).empty(),
          "Families without a point have no cone to fit");
  auto bad = factory;
  bad.at(at(ToolFamily::chamfer)).tip_angle = 0;
  require(tool_defaults_error(bad).text == std::string("chamfer: ") + angle_reason,
          "A defaults error names the family");

  // Overrides: each by itself, not against each other.
  for (const auto member : {&ToolOverrides::cutting_length, &ToolOverrides::shank_diameter,
                            &ToolOverrides::overall_length, &ToolOverrides::stickout}) {
    for (const double inside : {0.001, 10000.0}) {
      ToolOverrides o;
      o.*member = inside;
      require(tool_overrides_error(o, ToolFamily::generic).empty(), "An override length at its bound is valid");
    }
    for (const double outside : {below(0.001), above(10000.0), nan, infinity, -5.0}) {
      ToolOverrides o;
      o.*member = outside;
      require(tool_overrides_error(o, ToolFamily::generic).text == length_reason,
              "An override length out of range");
    }
  }
  for (const double inside : {1.0, 179.0}) {
    ToolOverrides o;
    o.tip_angle = inside;
    require(tool_overrides_error(o, ToolFamily::generic).empty(), "An override angle at its bound is valid");
  }
  for (const double outside : {below(1.0), above(179.0), nan}) {
    ToolOverrides o;
    o.tip_angle = outside;
    require(tool_overrides_error(o, ToolFamily::generic).text == angle_reason,
            "An override angle out of range");
  }
  ToolOverrides apart;
  apart.cutting_length = 500;
  apart.stickout = 1;
  require(tool_overrides_error(apart, ToolFamily::generic).empty() && tool_overrides_error({}, ToolFamily::generic).empty() &&
              ToolOverrides{}.empty() && !apart.empty(),
          "Overrides are not judged against each other: the resolved model is, when drawn");

  // The tool row.
  constexpr int largest = std::numeric_limits<int>::max();
  require(tool_binding_error({1, 0, 0, ""}).empty() &&
              tool_binding_error({largest, largest, 1e300, std::string(255, 'x')}).empty(),
          "The smallest and the largest tool row");
  require(!tool_binding_error({0, 0, 6, ""}).empty() && !tool_binding_error({-1, 0, 6, ""}).empty(),
          "A tool number must be above zero");
  require(!tool_binding_error({1, -1, 6, ""}).empty(), "A pocket must not be below zero");
  for (const double diameter : {below(0.0), -6.0, nan, infinity})
    require(!tool_binding_error({1, 1, diameter, ""}).empty(),
            "A diameter must be finite and not below zero");
  require(!tool_binding_error({1, 1, 6, std::string(256, 'x')}).empty(),
          "A description of 256 bytes is too long");
  std::string euros;
  for (int i = 0; i < 85; ++i)
    euros += "€"; // three bytes each
  require(euros.size() == 255 && tool_binding_error({1, 1, 6, euros}).empty() &&
              !tool_binding_error({1, 1, 6, euros + "€"}).empty(),
          "The limit counts UTF-8 bytes, not characters");
  for (const auto& comment :
       {std::string("a\nb"), std::string("a\rb"), std::string("a\0b", 3), std::string(1, '\0')})
    require(!tool_binding_error({1, 1, 6, comment}).empty(), "No NUL or line break");
  require(tool_binding_error({1, 1, 6, "a\tb \"quoted\" \\ é"}).empty(),
          "Other characters are the tool table's business");

  // The document.
  ToolModelDocument document{factory, {}};
  for (int number = 1; number <= 1001; ++number)
    document.assignments.push_back({row(number), ToolFamily::generic, {}});
  require(tool_document_error(document).empty(), "1001 assignments are allowed");
  document.assignments.push_back({row(1002), ToolFamily::generic, {}});
  require(has(tool_document_error(document).text, "more than 1001"), "1002 are not");
  document.assignments.resize(2);
  document.assignments.back().tool.number = 1;
  document.assignments.back().tool.comment = "another row";
  require(has(tool_document_error(document).text, "Tool 1: It is assigned more than once."),
          "A tool number is assigned once");

  // The commands.
  require(tool_model_command_error(defaults(0, factory)).empty() &&
              has(tool_model_command_error(defaults(0, bad)).text, "chamfer"),
          "A defaults command is judged as defaults are");
  require(tool_model_command_error(assign(0, 1)).empty() &&
              tool_model_command_error(remove(0, 1)).empty(),
          "An assignment and a removal");
  require(!tool_model_command_error(
               SetToolModelAssignment{machine, 0, row(1), std::nullopt, apart})
               .empty(),
          "A removal with overrides is refused");
  require(!tool_model_command_error(SetToolModelAssignment{machine, 0, row(0), {}, {}}).empty(),
          "A removal still needs a valid tool row");

  // The machine ID.
  for (const std::string_view id :
       {"a", "Z", "0", "mill-1", "G0759", "a_b-c", "0-_",
        "a123456789012345678901234567890123456789012345678901234567890123"})
    require(valid_machine_id(id), std::format("{} is a machine ID", id));
  for (const std::string_view id :
       {"", "-a", "_a", "a.b", "a b", "a/b", "../x", "a\n", " a", "a ", "é", "a:b",
        "a1234567890123456789012345678901234567890123456789012345678901234"})
    require(!valid_machine_id(id), std::format("\"{}\" is not a machine ID", id));
  require(!valid_machine_id(std::string_view{"a\0b", 3}), "A NUL is not part of a machine ID");
}

void locations() {
  const auto none = resolve_tool_model_location({{}, "/data", "/xdg", "/home/cnc"});
  require(none.availability == ToolModelAvailability::session_only && !none.machine_id &&
              none.directory.empty() && has(none.reason, "MACHINE_ID"),
          "No machine ID: session only, whatever else is configured");
  for (const auto* id : {"", "../other", "a/b", "mill 1", "-mill", "mill.1", "/abs"}) {
    const auto invalid = resolve_tool_model_location({id, "/data", "/xdg", "/home/cnc"});
    require(invalid.availability == ToolModelAvailability::unavailable && !invalid.machine_id &&
                invalid.directory.empty() && has(invalid.reason, "MACHINE_ID is not valid"),
            std::format("Machine ID \"{}\": unavailable, and no directory at all", id));
  }
  for (const auto* relative : {"", "data", "./data", "../data", "~/data", " /data"}) {
    const auto refused = resolve_tool_model_location({"mill-1", relative, "/xdg", "/home/cnc"});
    require(refused.availability == ToolModelAvailability::unavailable &&
                refused.machine_id == "mill-1" && refused.directory.empty() &&
                has(refused.reason, "DATA_DIRECTORY is not an absolute path"),
            std::format("DATA_DIRECTORY \"{}\": unavailable, no fallback to another root",
                        relative));
  }
  const auto configured = resolve_tool_model_location({"mill-1", "/srv/cnc", "/xdg", "/home/cnc"});
  require(configured.availability == ToolModelAvailability::available &&
              configured.directory == "/srv/cnc/mill-1" && configured.reason.empty() &&
              configured.machine_id == "mill-1",
          "DATA_DIRECTORY is the root, the machine ID appended");
  require(resolve_tool_model_location({"mill-1", {}, "/xdg", "/home/cnc"}).directory ==
              "/xdg/coffeecnc/mill-1",
          "Else an absolute XDG_DATA_HOME");
  for (const auto* relative : {"", "xdg", "./xdg"})
    require(resolve_tool_model_location({"mill-1", {}, relative, "/home/cnc"}).directory ==
                "/home/cnc/.local/share/coffeecnc/mill-1",
            "A relative XDG_DATA_HOME is not used: the per-user fallback is");
  require(resolve_tool_model_location({"mill-1", {}, {}, "/home/cnc"}).directory ==
              "/home/cnc/.local/share/coffeecnc/mill-1",
          "Without XDG_DATA_HOME: ~/.local/share");
  for (const auto& home : {std::optional<std::string>{}, std::optional<std::string>{"cnc"},
                           std::optional<std::string>{""}}) {
    const auto nowhere = resolve_tool_model_location({"mill-1", {}, "xdg", home});
    require(nowhere.availability == ToolModelAvailability::unavailable &&
                nowhere.directory.empty() && nowhere.machine_id == "mill-1" &&
                has(nowhere.reason, "DATA_DIRECTORY"),
            "No absolute path to fall back on: unavailable, never the working directory");
  }
  require(location("Mill").directory == "/data/Mill" && location("mill").directory == "/data/mill",
          "Machine IDs differ by case, and so do their directories");
}

auto bits(double value) -> std::uint64_t { return std::bit_cast<std::uint64_t>(value); }

void wire_text() {
  ToolOverrides overrides;
  overrides.stickout = 30;
  const ToolModelDocument document{factory_tool_defaults(),
                                   {{{3, 3, 6, "End mill 6 mm"}, ToolFamily::end_mill, overrides}}};
  const ToolModels available{"G0759", 2, ToolModelAvailability::available, "", document};
  const auto text = encode_tool_models(available);
  require(text.has_value(), "An available snapshot encodes");
  const auto message = Json::parse(text.value_or("null"));
  require(message.size() == 6 && message.at("type") == "tool_models" &&
              message.at("machine_id") == "G0759" && message.at("revision") == 2 &&
              message.at("revision").is_number_integer() &&
              message.at("availability") == "available" && message.at("reason") == "",
          "The snapshot has the contract's six fields");
  const auto& sent = message.at("document");
  require(sent.size() == 3 && sent.at("version") == 1 && sent.at("defaults").size() == 8 &&
              sent.at("defaults").at("drill") ==
                  Json{{"diameter", 10},      {"cuttingLength", 60}, {"shankDiameter", 10},
                       {"overallLength", 100}, {"stickout", 75},      {"tipAngle", 118}} &&
              sent.at("assignments") ==
                  Json::array({{{"tool",
                                 {{"number", 3},
                                  {"pocket", 3},
                                  {"diameter", 6},
                                  {"comment", "End mill 6 mm"}}},
                                {"family", "end-mill"},
                                {"overrides", {{"stickout", 30}}}}}),
          "The document is version, defaults by family and assignments");
  const auto session = Json::parse(
      encode_tool_models({{}, 0, ToolModelAvailability::session_only, "why", {}}).value_or("0"));
  require(session == Json{{"type", "tool_models"}, {"machine_id", nullptr}, {"revision", 0},
                          {"availability", "session_only"}, {"reason", "why"},
                          {"document", nullptr}},
          "A session-only snapshot");
  // The variants the shared fixtures have no example of, as exact text.
  const auto factory_json = Json::parse(
      R"({"end-mill":{"diameter":10,"cuttingLength":22,"shankDiameter":10,"overallLength":72,"stickout":35,"tipAngle":90},)"
      R"("ball-mill":{"diameter":10,"cuttingLength":20,"shankDiameter":10,"overallLength":72,"stickout":35,"tipAngle":90},)"
      R"("drill":{"diameter":10,"cuttingLength":60,"shankDiameter":10,"overallLength":100,"stickout":75,"tipAngle":118},)"
      R"("spot-drill":{"diameter":10,"cuttingLength":15,"shankDiameter":10,"overallLength":60,"stickout":30,"tipAngle":90},)"
      R"("chamfer":{"diameter":10,"cuttingLength":12,"shankDiameter":10,"overallLength":60,"stickout":30,"tipAngle":90},)"
      R"("face-mill":{"diameter":50,"cuttingLength":12,"shankDiameter":20,"overallLength":70,"stickout":40,"tipAngle":90},)"
      R"("generic":{"diameter":10,"cuttingLength":20,"shankDiameter":10,"overallLength":70,"stickout":35,"tipAngle":90},)"
      R"("touch-probe":{"diameter":4,"shankDiameter":12,"bodyDiameter":28,"bodyLength":32,"stylusLength":20}})");
  const auto fresh = encode_tool_models(
      {"mill-1", 0, ToolModelAvailability::available, "",
       ToolModelDocument{factory_tool_defaults(), {}}});
  auto fresh_json = Json::parse(
      R"({"type":"tool_models","machine_id":"mill-1","revision":0,"availability":"available",)"
      R"("reason":"","document":{"version":1,"defaults":null,"assignments":[]}})");
  fresh_json["document"]["defaults"] = factory_json;
  require(fresh && Json::parse(*fresh) == fresh_json,
          "Available at revision 0: the factory defaults and no assignments");
  require(fresh == std::string(
                       R"({"availability":"available","document":{"assignments":[],"defaults":{)"
                       R"("ball-mill":{"cuttingLength":20.0,"diameter":10.0,"overallLength":72.0,"shankDiameter":10.0,"stickout":35.0,"tipAngle":90.0},)"
                       R"("chamfer":{"cuttingLength":12.0,"diameter":10.0,"overallLength":60.0,"shankDiameter":10.0,"stickout":30.0,"tipAngle":90.0},)"
                       R"("drill":{"cuttingLength":60.0,"diameter":10.0,"overallLength":100.0,"shankDiameter":10.0,"stickout":75.0,"tipAngle":118.0},)"
                       R"("end-mill":{"cuttingLength":22.0,"diameter":10.0,"overallLength":72.0,"shankDiameter":10.0,"stickout":35.0,"tipAngle":90.0},)"
                       R"("face-mill":{"cuttingLength":12.0,"diameter":50.0,"overallLength":70.0,"shankDiameter":20.0,"stickout":40.0,"tipAngle":90.0},)"
                       R"("generic":{"cuttingLength":20.0,"diameter":10.0,"overallLength":70.0,"shankDiameter":10.0,"stickout":35.0,"tipAngle":90.0},)"
                       R"("spot-drill":{"cuttingLength":15.0,"diameter":10.0,"overallLength":60.0,"shankDiameter":10.0,"stickout":30.0,"tipAngle":90.0},)"
                       R"("touch-probe":{"bodyDiameter":28.0,"bodyLength":32.0,"diameter":4.0,"shankDiameter":12.0,"stylusLength":20.0}},)"
                       R"("version":1},"machine_id":"mill-1","reason":"","revision":0,"type":"tool_models"})"),
          "and these are its bytes: keys in alphabetical order, whole numbers with .0");
  const auto inherited = encode_tool_models(
      {"mill-1", 4, ToolModelAvailability::available, "",
       ToolModelDocument{factory_tool_defaults(), {{{2, 0, 0, ""}, ToolFamily::generic, {}}}}});
  require(inherited &&
              Json::parse(*inherited).at("document").at("assignments") ==
                  Json::parse(R"([{"tool":{"number":2,"pocket":0,"diameter":0,"comment":""},)"
                              R"("family":"generic","overrides":{}}])") &&
              has(*inherited,
                  R"("assignments":[{"family":"generic","overrides":{},"tool":{"comment":"","diameter":0.0,"number":2,"pocket":0}}])"),
          "An assignment that inherits everything: overrides is an empty object");
  require(encode_tool_models({{}, 0, ToolModelAvailability::unavailable,
                              "[COFFEECNC] MACHINE_ID is not valid", {}}) ==
              std::string(R"({"availability":"unavailable","document":null,"machine_id":null,)"
                          R"("reason":"[COFFEECNC] MACHINE_ID is not valid","revision":0,)"
                          R"("type":"tool_models"})"),
          "Unavailable without a machine ID: machine_id is null");
  require(Json::parse(encode_tool_models({"m", 0, ToolModelAvailability::unavailable,
                                          "/data/\xff/x cannot be read", {}})
                          .value_or("0"))
              .at("reason")
              .get<std::string>()
              .starts_with("/data/"),
          "A reason that is not UTF-8 is still sent");

  // Refused, never repaired.
  for (const auto& broken :
       {ToolModels{{}, 2, ToolModelAvailability::available, "", document},
        ToolModels{"m", 2, ToolModelAvailability::available, "", {}},
        ToolModels{"m", 1, ToolModelAvailability::unavailable, "", {}},
        ToolModels{"m", 0, ToolModelAvailability::session_only, "", document},
        ToolModels{"m", last_revision + 1, ToolModelAvailability::available, "", document}})
    require(!encode_tool_models(broken), "A snapshot that contradicts itself is not sent");
  require(encode_tool_models({"m", last_revision, ToolModelAvailability::available, "", document})
              .has_value(),
          "Revision 2^53 - 1 is the last one sent");
  for (const double value : {nan, infinity, -infinity}) {
    auto in_defaults = available;
    doc(in_defaults).defaults.at(at(ToolFamily::generic)).overall_length = value;
    auto in_override = available;
    doc(in_override).assignments.front().overrides.tip_angle = value;
    auto in_row = available;
    doc(in_row).assignments.front().tool.diameter = value;
    for (const auto& broken : {in_defaults, in_override, in_row}) {
      const auto refused = encode_tool_models(broken);
      require(!refused && has(refused.error().text, "not a finite number"),
              "A number that is not finite is not sent");
      require(!encode_tool_model_file({"m", 1, doc(broken)}), "nor saved");
    }
  }
  auto not_text = available;
  doc(not_text).assignments.front().tool.comment = "\xff\xfe";
  require(!encode_tool_models(not_text) && !encode_tool_model_file({"m", 1, doc(not_text)}),
          "A description that is not UTF-8 is refused, not replaced");

  // What is saved reads back as the same doubles and the same bytes of text.
  ToolModelDocument exact{other_defaults(), {}};
  const std::vector<double> diameters{0.1 + 0.2,       6.35,        1.0 / 3.0, 0.25 * 25.4,
                                      0.125 * 25.4,    0.001,       above(6.0), below(6.0),
                                      5e-324,          1e300,       0.0,       9999.999999999998,
                                      3.0000000000000004};
  int number = 1;
  for (const double diameter : diameters) {
    ToolOverrides o;
    o.stickout = above(30.0 + number);
    o.tip_angle = 117.99999999999999;
    exact.assignments.push_back(
        {{number, number - 1, diameter, std::format("Ø{} \"tool\" \\ \t {}", number, "é€")},
         ToolFamily::drill, o});
    ++number;
  }
  const auto file_text = saved(7, exact, "Mill_7");
  const auto read = decode_tool_model_file(file_text);
  require(read.has_value(), "A saved file reads back: " + read.error_or(LocalizedReason{}).text);
  if (!read)
    return;
  require(read->machine_id == "Mill_7" && read->revision == 7 && read->document == exact,
          "The saved file reads back equal");
  for (std::size_t i = 0; i < diameters.size(); ++i) {
    const auto& tool = read->document.assignments.at(i);
    require(bits(tool.tool.diameter) == bits(diameters.at(i)) &&
                bits(tool.overrides.stickout.value_or(0)) ==
                    bits(above(31.0 + static_cast<double>(i))) &&
                bits(tool.overrides.tip_angle.value_or(0)) == bits(117.99999999999999),
            "Every saved number reads back bit for bit");
  }
  require(bits(read->document.defaults.at(at(ToolFamily::end_mill)).stickout) == bits(odd_length),
          "also in the defaults");
  require(saved(7, read->document, "Mill_7") == file_text, "Saving it again gives the same bytes");
  const auto envelope = Json::parse(file_text);
  require(envelope.size() == 3 && envelope.at("machine_id") == "Mill_7" &&
              envelope.at("revision") == 7 && envelope.at("document").at("version") == 1,
          "The file is machine_id, revision and the document");
}

// The valid file `text` with one change made to its JSON.
auto altered(const std::string& text, const std::function<void(Json&)>& change) -> std::string {
  auto json = Json::parse(text);
  change(json);
  return json.dump();
}

// The first assignment of a saved file's JSON, to be changed.
auto first_saved(Json& saved_file) -> Json& {
  auto& assignments = saved_file.at("document").at("assignments");
  return assignments.at(0);
}

void file_refusals() {
  ToolOverrides overrides;
  overrides.stickout = 30;
  const ToolModelDocument document{
      factory_tool_defaults(),
      {{row(5), ToolFamily::drill, overrides}, {row(2), ToolFamily::generic, {}}}};
  // Not in rising order in the file (5, then 2): accepted, and put in order.
  const auto good = altered(saved(3, {document.defaults, {}}), [&](Json& json) -> void {
    json["document"]["assignments"] =
        Json::parse(saved(3, document)).at("document").at("assignments");
  });
  const auto read = decode_tool_model_file(good);
  require(read && read->document.assignments.size() == 2 &&
              read->document.assignments.front().tool.number == 2 &&
              read->document.assignments.back().overrides.stickout == 30,
          "Assignments come back in rising tool order");

  const auto refused = [&](const std::string& text, std::string_view part,
                           const std::string& what) -> void {
    const auto result = decode_tool_model_file(text);
    require(!result && has(result.error().text, part),
            std::format("{}: refused with \"{}\", got \"{}\"", what, part,
                        result ? "a document" : result.error().text));
  };
  const auto change = [&](const std::function<void(Json&)>& edit, std::string_view part,
                          const std::string& what) -> void {
    refused(altered(good, edit), part, what);
  };
  refused("", "not valid JSON", "An empty file");
  refused("{", "not valid JSON", "Half a file");
  refused(good + "x", "not valid JSON", "Text after the document");
  refused("[]", "not a tool model document", "An array");
  refused("{}", "not a tool model document", "An empty object");
  refused(R"({"machine_id":"mill-1","revision":1,"document":[]})", "not a tool model document",
          "A document that is not an object");
  refused(R"({"machine_id":"a","machine_id":"mill-1","revision":1,"document":{"version":1}})",
          "duplicate key", "A duplicate key");
  refused(std::string(9, '[') + std::string(9, ']'), "nested too deeply", "Deep nesting");
  refused(std::string(max_tool_model_bytes + 1, ' '), "larger than 1 MiB", "An oversized text");

  change([](Json& j) -> void { j["document"]["version"] = 2; },
         "version 2 is not supported", "A later version");
  change([](Json& j) -> void {
    j["document"]["version"] = 3;
    j["document"]["jobs"] = Json::array();
    j["checksum"] = "x";
  }, "version 3 is not supported", "A later version with fields of its own");
  for (const auto& version : {Json(0), Json(-1), Json(1.5), Json("1"), Json(nullptr), Json(true)})
    change([&](Json& j) -> void { j["document"]["version"] = version; }, "version",
           "Version " + version.dump());
  change([](Json& j) -> void { j["document"].erase("version"); }, "not a tool model document",
         "No version");

  change([](Json& j) -> void { j["extra"] = 1; }, "unknown field", "An unknown file field");
  change([](Json& j) -> void { j["document"]["jobs"] = 1; }, "unknown field",
         "An unknown document field");
  for (const auto* field : {"machine_id", "revision"})
    change([&](Json& j) -> void { j.erase(field); }, "lacks the field", "A missing file field");
  for (const auto* field : {"defaults", "assignments"})
    change([&](Json& j) -> void { j["document"].erase(field); }, "lacks the field",
           "A missing document field");
  for (const auto& id : {Json("../x"), Json(""), Json(7), Json(nullptr)})
    change([&](Json& j) -> void { j["machine_id"] = id; }, "machine_id",
           "Machine ID " + id.dump());
  for (const auto& revision : {Json(0), Json(-1), Json(1.5), Json("3"), Json(nullptr),
                               Json(9007199254740992.0), Json(1e300)})
    change([&](Json& j) -> void { j["revision"] = revision; }, "revision",
           "Revision " + revision.dump());
  require(decode_tool_model_file(altered(good, [](Json& j) -> void {
            j["revision"] = last_revision;
          })).transform([](const ToolModelFile& f) -> std::uint64_t { return f.revision; }) ==
              last_revision &&
              decode_tool_model_file(altered(good, [](Json& j) -> void { j["revision"] = 1; }))
                  .has_value(),
          "Revisions 1 and 2^53 - 1 are the first and the last");

  change([](Json& j) -> void { j["document"]["defaults"].erase("generic"); },
         "lacks the family generic", "A missing family");
  change([](Json& j) -> void { j["document"]["defaults"]["lathe"] = 1; }, "unknown family",
         "An unknown family");
  change([](Json& j) -> void { j["document"]["defaults"] = Json::array(); },
         "defaults must be an object", "Defaults that are not an object");
  change([](Json& j) -> void { j["document"]["defaults"]["drill"].erase("tipAngle"); },
         "drill lacks the field tipAngle", "A missing dimension");
  change([](Json& j) -> void { j["document"]["defaults"]["drill"]["flutes"] = 2; },
         "drill has an unknown field", "An unknown dimension");
  change([](Json& j) -> void { j["document"]["defaults"]["drill"]["stickout"] = "75"; },
         "drill stickout must be a number", "A dimension that is not a number");
  change([](Json& j) -> void { j["document"]["defaults"]["drill"]["tipAngle"] = 180; },
         "drill: Included tip angle", "Defaults out of range");
  change([](Json& j) -> void { j["document"]["defaults"]["ball-mill"]["cuttingLength"] = 4; },
         "ball-mill: Cutting length must include", "Defaults that break a rule");

  change([](Json& j) -> void { j["document"]["assignments"] = Json::object(); },
         "assignments must be an array", "Assignments that are not an array");
  change([&](Json& j) -> void { first_saved(j)["identity"] = "x"; }, "An assignment has an unknown",
         "An unknown assignment field");
  change([&](Json& j) -> void { first_saved(j).erase("overrides"); }, "An assignment lacks",
         "An assignment without overrides");
  change([&](Json& j) -> void { first_saved(j)["family"] = "lathe"; }, "family must be a supported",
         "An unknown family name");
  change([&](Json& j) -> void { first_saved(j)["family"] = nullptr; }, "family must be a supported",
         "A saved assignment without a family");
  change([&](Json& j) -> void { first_saved(j)["overrides"]["diameter"] = 6; },
         "overrides has an unknown field", "A diameter override");
  change([&](Json& j) -> void { first_saved(j)["overrides"]["stickout"] = nullptr; },
         "overrides stickout must be a number", "A null override");
  change([&](Json& j) -> void { first_saved(j)["overrides"]["stickout"] = 0; }, "Dimensions must be",
         "An override out of range");
  change([&](Json& j) -> void { first_saved(j)["overrides"] = Json::array(); },
         "overrides must be an object", "Overrides that are not an object");
  change([&](Json& j) -> void { first_saved(j)["tool"]["length"] = 50; }, "tool has an unknown field",
         "A tool row with a length");
  change([&](Json& j) -> void { first_saved(j)["tool"].erase("pocket"); }, "tool lacks the field pocket",
         "A tool row without a pocket");
  for (const auto& number : {Json(0), Json(-3), Json(1.5), Json(2147483648.0), Json("2")})
    change([&](Json& j) -> void { first_saved(j)["tool"]["number"] = number; }, "ool",
           "Tool number " + number.dump());
  for (const auto& pocket : {Json(-1), Json(0.5), Json(2147483648.0), Json(nullptr)})
    change([&](Json& j) -> void { first_saved(j)["tool"]["pocket"] = pocket; }, "ocket",
           "Pocket " + pocket.dump());
  require(decode_tool_model_file(altered(good, [&](Json& j) -> void {
            first_saved(j)["tool"]["number"] = 2147483647;
            first_saved(j)["tool"]["pocket"] = 2147483647;
          })).has_value() &&
              decode_tool_model_file(altered(good, [&](Json& j) -> void {
                first_saved(j)["tool"]["pocket"] = 0;
                first_saved(j)["tool"]["diameter"] = 0;
              })).has_value(),
          "The largest tool number and pocket, pocket 0 and diameter 0 are valid");
  change([&](Json& j) -> void { first_saved(j)["tool"]["diameter"] = -0.5; }, "diameter",
         "A negative diameter");
  change([&](Json& j) -> void { first_saved(j)["tool"]["diameter"] = "6"; }, "diameter must be a number",
         "A diameter that is not a number");
  change([&](Json& j) -> void { first_saved(j)["tool"]["comment"] = 5; }, "comment must be a string",
         "A description that is not text");
  change([&](Json& j) -> void { first_saved(j)["tool"]["comment"] = std::string(256, 'x'); },
         "longer than 255 bytes", "A description of 256 bytes");
  change([&](Json& j) -> void { first_saved(j)["tool"]["comment"] = "a\nb"; }, "line break",
         "A description with a line break");
  change([&](Json& j) -> void { first_saved(j)["tool"]["comment"] = std::string("a\0b", 3); },
         "NUL", "A description with a NUL");
  change([&](Json& j) -> void {
    j["document"]["assignments"][1]["tool"]["number"] = first_saved(j)["tool"]["number"];
  }, "assigned more than once", "A tool assigned twice");
  require(decode_tool_model_file(altered(good, [&](Json& j) -> void {
            first_saved(j)["tool"]["comment"] = std::string(255, 'x');
          })).has_value(),
          "A description of 255 bytes is valid");

  ToolModelDocument full{factory_tool_defaults(), {}};
  for (int number = 1; number <= 1001; ++number)
    full.assignments.push_back({{number, 0, 0, ""}, ToolFamily::generic, {}});
  const auto full_text = saved(1, full);
  require(decode_tool_model_file(full_text).has_value(), "A file with 1001 assignments reads");
  refused(altered(full_text, [](Json& j) -> void {
            auto one_more = j["document"]["assignments"][0];
            one_more["tool"]["number"] = 5000;
            j["document"]["assignments"].push_back(one_more);
          }),
          "more than 1001", "A file with 1002 assignments");
}

auto steps(const FakeToolModelFiles& files, std::size_t from = 0) -> std::string {
  std::string joined;
  for (std::size_t i = from; i < files.calls.size(); ++i)
    joined += (joined.empty() ? "" : ", ") + files.calls.at(i);
  return joined;
}

// An archive of machine `mill-1` under /data on a disk of its own.
struct Rig {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  ToolModelArchive archive{files, location()};

  explicit Rig(std::optional<std::string> content = {}) {
    if (content)
      disk.files[file] = std::move(*content);
    archive.open();
  }
  [[nodiscard]] auto revision() const -> std::uint64_t { return archive.models().revision; }
  [[nodiscard]] auto available() const -> bool {
    return archive.models().availability == ToolModelAvailability::available;
  }
  // The file as the archive would read it now.
  [[nodiscard]] auto on_disk() const -> std::optional<ToolModelFile> {
    const auto found = disk.files.find(file);
    if (found == disk.files.end())
      return {};
    const auto read = decode_tool_model_file(found->second);
    require(read.has_value(), "The saved file is valid: " + read.error_or(LocalizedReason{}).text);
    return read ? std::optional<ToolModelFile>{*read} : std::nullopt;
  }
  void accepted(const ToolModelCommand& command, const std::string& what) {
    const auto before = revision();
    const auto reply = archive.apply(command);
    require(reply.outcome == CommandOutcome::accepted && reply.reason.empty() && reply.snapshot &&
                revision() == before + 1,
            what + ": accepted, with the snapshot of the next revision; " + reply.reason);
    const auto saved_now = on_disk();
    require(saved_now && saved_now->revision == revision() && saved_now->machine_id == machine &&
                saved_now->document == archive.models().document &&
                Json::parse(archive.snapshot()).at("revision") == revision() &&
                !disk.files.contains(temporary),
            what + ": the file holds what the snapshot shows, and no temporary file is left");
  }
};

void archive_reads() {
  {
    Rig rig;
    const auto& models = rig.archive.models();
    require(models.availability == ToolModelAvailability::available && models.revision == 0 &&
                models.machine_id == machine && models.reason.empty() && models.document &&
                models.document->defaults == factory_tool_defaults() &&
                models.document->assignments.empty(),
            "No file: available, revision 0, the factory defaults, no assignments");
    require(steps(rig.files) == opening,
            "Opening makes the directories durable, takes the lock and reads: " +
                steps(rig.files));
    require(rig.disk.directories == std::set<std::string>{"/", "/data", "/data/mill-1"},
            "The data root and the machine directory were made");
    require(!rig.disk.files.contains(file) && !rig.disk.files.contains(temporary) &&
                rig.disk.files.size() == 1 && rig.disk.files.contains(directory + "/lock"),
            "Reading makes no document: only the directory and its lock file exist");
    const auto snapshot = Json::parse(rig.archive.snapshot());
    require(snapshot.at("availability") == "available" && snapshot.at("revision") == 0 &&
                snapshot.at("document").at("assignments").empty(),
            "The snapshot says the same");
    rig.archive.close();
    require(rig.disk.locked.empty(), "Closing releases the lock");
  }
  for (const auto& settings :
       {ToolModelSettings{{}, "/data", {}, {}}, ToolModelSettings{"../x", "/data", {}, {}},
        ToolModelSettings{"mill-1", "data", {}, {}}}) {
    FakeDisk disk;
    FakeToolModelFiles files{disk};
    const auto where = resolve_tool_model_location(settings);
    ToolModelArchive archive{files, where};
    archive.open();
    const auto reply = archive.apply(assign(0, 1));
    require(archive.models().availability == where.availability &&
                archive.models().reason == where.reason && !archive.models().document &&
                archive.models().revision == 0 && archive.models().machine_id == where.machine_id,
            "A location that is not available is the archive's state");
    require(reply.outcome == CommandOutcome::rejected && reply.reason == where.reason &&
                !reply.snapshot,
            "and every command is rejected with its reason");
    require(files.calls.empty() && disk.files.empty() &&
                disk.directories == std::set<std::string>{"/"},
            "Nothing is locked, read or made: " + steps(files));
    require(Json::parse(archive.snapshot()).at("availability") ==
                (settings.machine_id ? "unavailable" : "session_only"),
            "The snapshot says which");
  }

  const ToolModelDocument document{other_defaults(), {{row(4), ToolFamily::chamfer, {}}}};
  const auto good = saved(12, document);
  {
    Rig rig{good};
    require(rig.available() && rig.revision() == 12 && rig.archive.models().document == document,
            "A saved file is what is shown");
  }
  // Exactly 1 MiB is read; one byte more is not.
  {
    Rig rig{good + std::string(max_tool_model_bytes - good.size(), ' ')};
    require(rig.available() && rig.revision() == 12, "A file of exactly 1 MiB is read");
  }
  struct Unusable {
    std::string name;
    std::string content;
    std::string reason;
  };
  const std::vector<Unusable> unusable{
      {"oversized", good + std::string(max_tool_model_bytes - good.size() + 1, ' '),
       "is larger than 1 MiB"},
      {"corrupt", good.substr(0, good.size() / 2), "is not usable: It is not valid JSON."},
      {"empty", "", "is not usable: It is not valid JSON."},
      {"future", altered(good, [](Json& j) -> void { j["document"]["version"] = 2; }),
       "is not usable: Its version 2 is not supported by this CoffeeCNC."},
      {"unknown field", altered(good, [](Json& j) -> void { j["document"]["jobs"] = 1; }),
       "is not usable: document has an unknown field."},
      {"another machine's", saved(12, document, "mill-2"),
       "belongs to machine mill-2, not to mill-1"},
      {"another machine's by case", saved(12, document, "Mill-1"),
       "belongs to machine Mill-1, not to mill-1"},
  };
  for (const auto& [name, content, reason] : unusable) {
    Rig rig{content};
    const auto& models = rig.archive.models();
    require(models.availability == ToolModelAvailability::unavailable && models.revision == 0 &&
                !models.document && models.machine_id == machine &&
                has(models.reason, file) && has(models.reason, reason) &&
                has(models.reason, "It is not changed: move or remove it and restart"),
            std::format("A file that is {}: unavailable, naming the file: {}", name,
                        models.reason));
    for (const auto& command : {defaults(0, other_defaults()), assign(0, 1), remove(0, 4),
                                defaults(12, other_defaults())}) {
      const auto reply = rig.archive.apply(command);
      require(reply.outcome == CommandOutcome::rejected && reply.reason == models.reason,
              std::format("A file that is {}: every command is rejected with that reason", name));
    }
    require(rig.disk.files.at(file) == content && rig.disk.files.size() == 2 &&
                steps(rig.files) == opening,
            std::format("A file that is {} is never written, renamed or removed: {}", name,
                        steps(rig.files)));
  }
  {
    const auto unknown_holder = altered(good, [](Json& value) {
      value["document"]["assignments"][0]["holder"] = "retired-holder";
    });
    Rig rig{unknown_holder};
    const auto state = Json::parse(encode_tool_models_reason(rig.archive.models()).value());
    const auto localization = state.at("localization");
    const auto tool = localization.at("params_localization").at("detail");
    const auto holder = tool.at("params_localization").at("detail");
    require(localization.at("key") == "backend.toolModelStore.notUsable" &&
                tool.at("key") == "backend.toolModels.toolProblem" &&
                holder.at("key") == "backend.toolModels.unknownHolder" &&
                has(rig.archive.models().reason, "Unknown tool holder."),
            "A saved model wraps the typed codec detail and retains the English fallback");
  }
  {
    FakeDisk disk;
    disk.files[file] = good;
    FakeToolModelFiles files{disk};
    files.failures["read"] = 1;
    ToolModelArchive archive{files, location()};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::unavailable &&
                has(archive.models().reason, file + " cannot be read: it cannot be read: "
                                                    "Input/output error"),
            "A file that cannot be read: unavailable: " + archive.models().reason);
    require(archive.apply(assign(12, 1)).outcome == CommandOutcome::rejected &&
                disk.files.at(file) == good && files.count("read") == 1,
            "and it stays so: nothing is tried again or written");
  }
  {
    FakeDisk disk;
    FakeToolModelFiles files{disk};
    files.failures["lock"] = 1;
    ToolModelArchive archive{files, location()};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::unavailable &&
                has(archive.models().reason, "/data/mill-1 cannot be opened") &&
                files.count("read") == 0 &&
                archive.apply(assign(0, 1)).outcome == CommandOutcome::rejected &&
                disk.files.empty(),
            "A directory that cannot be locked: unavailable, nothing read or written");
  }
}

// The directories are on the disk before the store says it is available.
void archive_directories() {
  // A deeper location, nothing of it there yet.
  const auto deep = resolve_tool_model_location({"mill-1", "/srv/cnc/data", {}, {}});
  const std::string made =
      "make_directory /srv, sync_directory /, make_directory /srv/cnc, sync_directory /srv, "
      "make_directory /srv/cnc/data, sync_directory /srv/cnc, "
      "make_directory /srv/cnc/data/mill-1, sync_directory /srv/cnc/data, "
      "lock /srv/cnc/data/mill-1, read /srv/cnc/data/mill-1/tool-models.json";
  {
    FakeDisk disk;
    FakeToolModelFiles files{disk};
    ToolModelArchive archive{files, deep};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::available &&
                archive.models().reason.empty() && steps(files) == made,
            "Every missing directory is made and its parent synchronised before the next, the "
            "machine directory last, then the lock: " + steps(files));
  }
  // What exists is neither made nor synchronised again.
  {
    FakeDisk disk;
    disk.directories = {"/", "/srv", "/srv/cnc"};
    FakeToolModelFiles files{disk};
    ToolModelArchive archive{files, deep};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::available &&
                steps(files) ==
                    "make_directory /srv, make_directory /srv/cnc, make_directory /srv/cnc/data, "
                    "sync_directory /srv/cnc, make_directory /srv/cnc/data/mill-1, "
                    "sync_directory /srv/cnc/data, lock /srv/cnc/data/mill-1, "
                    "read /srv/cnc/data/mill-1/tool-models.json",
            "Only the parents of directories made here are synchronised: " + steps(files));
  }
  // A failure at any level: unavailable, and nothing is locked, read or written afterwards.
  for (const auto* failing :
       {"sync_directory /", "sync_directory /srv", "sync_directory /srv/cnc",
        "sync_directory /srv/cnc/data", "make_directory /srv", "make_directory /srv/cnc/data",
        "make_directory /srv/cnc/data/mill-1"}) {
    FakeDisk disk;
    FakeToolModelFiles files{disk};
    files.failures[failing] = 1;
    ToolModelArchive archive{files, deep};
    archive.open();
    const auto& models = archive.models();
    require(models.availability == ToolModelAvailability::unavailable && !models.document &&
                has(models.reason, "/srv/cnc/data/mill-1 cannot be opened") &&
                has(models.reason, std::string(failing).starts_with("sync")
                                       ? "cannot be made durable"
                                       : "cannot be made"),
            std::format("A failure of {}: unavailable: {}", failing, models.reason));
    require(files.calls.back() == failing && files.count("lock") == 0 && files.count("read") == 0,
            std::format("A failure of {}: nothing follows it: {}", failing, steps(files)));
    const auto reply = archive.apply(SetToolModelAssignment{"mill-1", 0, row(1),
                                                            ToolFamily::generic, {}});
    require(reply.outcome == CommandOutcome::rejected && reply.reason == models.reason &&
                files.calls.back() == failing && disk.files.empty(),
            std::format("A failure of {}: no later save is attempted or claimed", failing));
  }
  // A machine directory that was there, open to other users: used, and said.
  for (const unsigned mode : {0755U, 0750U, 0701U, 0777U}) {
    FakeDisk disk;
    disk.directories = {"/", "/data", "/data/mill-1"};
    disk.modes["/data/mill-1"] = mode;
    disk.modes["/data"] = 0755;
    FakeToolModelFiles files{disk};
    ToolModelArchive archive{files, location()};
    archive.open();
    const auto note = std::format("The directory /data/mill-1 can be entered by other users "
                                  "(mode {:o}).",
                                  mode);
    require(archive.models().availability == ToolModelAvailability::available &&
                archive.models().reason == note && files.count("sync_directory") == 0,
            std::format("An existing machine directory of mode {:o} is used as it is, and the "
                        "snapshot says so: {}",
                        mode, archive.models().reason));
    require(archive.apply(assign(0, 1)).outcome == CommandOutcome::accepted &&
                archive.models().reason == note &&
                Json::parse(archive.snapshot()).at("reason") == note,
            "also after a save");
  }
  {
    FakeDisk disk;
    disk.directories = {"/", "/data", "/data/mill-1"};
    disk.modes["/data"] = 0777;
    FakeToolModelFiles files{disk};
    ToolModelArchive archive{files, location()};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::available &&
                archive.models().reason.empty(),
            "The mode of a data root that was there is the operator's: nothing is said");
  }
}

void archive_writes() {
  Rig rig;
  const auto mark = rig.files.calls.size();
  rig.accepted(defaults(0, other_defaults()), "The first defaults");
  require(steps(rig.files, mark) ==
              std::format("read {0}, open_temporary {1}, write {1}, sync {1}, rename {1} {0}, "
                          "sync_directory {2}",
                          file, temporary, directory),
          "A save is read, temporary file, write, fsync, rename, directory fsync: " +
              steps(rig.files, mark));
  require(rig.revision() == 1 && doc(rig.archive.models()).defaults == other_defaults() &&
              doc(rig.archive.models()).assignments.empty(),
          "Revision 1 holds the new defaults");

  ToolOverrides overrides;
  overrides.stickout = 30;
  overrides.tip_angle = 60;
  rig.accepted(assign(1, 7, ToolFamily::chamfer, overrides), "An assignment");
  rig.accepted(assign(2, 3), "A second one, of a lower tool");
  rig.accepted(assign(3, 12, ToolFamily::drill), "A third");
  const auto numbers = [&]() -> std::vector<int> {
    std::vector<int> found;
    std::ranges::transform(doc(rig.archive.models()).assignments, std::back_inserter(found),
                           [](const ToolModelAssignment& a) -> int { return a.tool.number; });
    return found;
  };
  require(numbers() == std::vector<int>{3, 7, 12} &&
              doc(rig.archive.models()).assignments.at(1) ==
                  ToolModelAssignment{row(7), ToolFamily::chamfer, overrides} &&
              doc(rig.archive.models()).defaults == other_defaults(),
          "Assignments are kept by tool number; the defaults stay");

  // Replacing one: the row and the model, nothing else.
  ToolBinding changed_row{7, 2, 8.0, "another cutter"};
  rig.accepted(SetToolModelAssignment{machine, 4, changed_row, ToolFamily::ball_mill, {}},
               "Replacing an assignment");
  require(numbers() == std::vector<int>{3, 7, 12} &&
              doc(rig.archive.models()).assignments.at(1) ==
                  ToolModelAssignment{changed_row, ToolFamily::ball_mill, {}} &&
              doc(rig.archive.models()).assignments.front().tool == row(3),
          "Only that tool's entry changed");
  rig.accepted(defaults(5, factory_tool_defaults()), "New defaults");
  require(numbers() == std::vector<int>{3, 7, 12} &&
              doc(rig.archive.models()).defaults == factory_tool_defaults(),
          "New defaults keep every assignment");
  rig.accepted(remove(6, 7), "A removal");
  require(numbers() == std::vector<int>{3, 12}, "Only that tool's entry is gone");
  rig.accepted(remove(7, 7), "Removing what is not assigned");
  require(numbers() == std::vector<int>{3, 12} && rig.revision() == 8,
          "changes nothing but the revision");

  // A stale base revision: the snapshot, then the rejection; nothing is written.
  const auto before = rig.disk.files.at(file);
  const auto calls = rig.files.calls.size();
  for (const std::uint64_t base : {std::uint64_t{7}, std::uint64_t{9}, std::uint64_t{0}}) {
    const auto reply = rig.archive.apply(assign(base, 20));
    require(reply.outcome == CommandOutcome::rejected && reply.snapshot &&
                has(reply.reason, std::format("since revision {} was shown: they are at revision 8",
                                              base)),
            "A stale base revision is rejected, with the snapshot: " + reply.reason);
  }
  require(rig.disk.files.at(file) == before && rig.revision() == 8 &&
              steps(rig.files, calls) == std::format("read {0}, read {0}, read {0}", file),
          "and nothing was written for it");
}

// The settings file changed by something else while this process runs: nothing of it is taken
// over and nothing is written over it. Storage is unavailable until the next start.
void archive_outside_changes() {
  const ToolModelDocument held{other_defaults(), {{row(4), ToolFamily::chamfer, {}}}};
  auto edited = held;
  edited.assignments.front().family = ToolFamily::drill;
  struct Change {
    std::string name;
    std::optional<std::string> content; // none: the file is removed
    std::uint64_t base;                 // the revision the command names
  };
  const std::vector<Change> changes{
      {"removed", std::nullopt, 5},
      {"removed, command based on revision 0", std::nullopt, 0},
      {"replaced with a lower revision", saved(3, held), 5},
      {"replaced with a lower revision, command based on it", saved(3, held), 3},
      {"replaced with a higher revision", saved(9, held), 5},
      {"replaced with a higher revision, command based on it", saved(9, held), 9},
      {"replaced with other content at the same revision", saved(5, edited), 5},
      {"replaced with text that is no document", std::string("{ not json"), 5},
      {"replaced with another machine's file", saved(5, held, "mill-2"), 5},
  };
  for (const auto& [name, content, base] : changes) {
    Rig rig{saved(5, held)};
    require(rig.available() && rig.revision() == 5, "The file as this process read it");
    if (content)
      rig.disk.files[file] = *content;
    else
      rig.disk.files.erase(file);
    const auto calls = rig.files.calls.size();
    const auto reply = rig.archive.apply(assign(base, 9));
    const auto& models = rig.archive.models();
    require(reply.outcome == CommandOutcome::rejected && reply.snapshot &&
                reply.reason == models.reason,
            std::format("File {}: the command is rejected, after the snapshot", name));
    require(models.availability == ToolModelAvailability::unavailable && models.revision == 0 &&
                !models.document && models.machine_id == machine && has(models.reason, file) &&
                has(models.reason, "was changed from outside while CoffeeCNC runs"),
            std::format("File {}: unavailable, never an available snapshot of what the file "
                        "holds now: {}",
                        name, models.reason));
    const auto snapshot = Json::parse(rig.archive.snapshot());
    require(snapshot.at("availability") == "unavailable" && snapshot.at("revision") == 0 &&
                snapshot.at("document").is_null(),
            std::format("File {}: the snapshot says so", name));
    require(steps(rig.files, calls) == "read " + file,
            std::format("File {}: one read and no write: {}", name, steps(rig.files, calls)));
    // Sticky: whatever is asked next, also what would fit the file as it is now.
    for (const std::uint64_t later : {std::uint64_t{0}, std::uint64_t{3}, std::uint64_t{5},
                                      std::uint64_t{9}}) {
      const auto again = rig.archive.apply(defaults(later, factory_tool_defaults()));
      require(again.outcome == CommandOutcome::rejected && again.reason == models.reason,
              std::format("File {}: every later command is rejected the same way", name));
    }
    require(rig.files.calls.size() == calls + 1 &&
                (content ? rig.disk.files.at(file) == *content : !rig.disk.files.contains(file)) &&
                !rig.disk.files.contains(temporary),
            std::format("File {}: it is not read again and never overwritten", name));
    // Until a new store instance: that one reads what is there.
    rig.archive.close();
    FakeToolModelFiles next_files{rig.disk};
    ToolModelArchive next{next_files, location()};
    next.open();
    const bool usable = !content || decode_tool_model_file(*content)
                                        .transform([](const ToolModelFile& f) -> bool {
                                          return f.machine_id == machine;
                                        })
                                        .value_or(false);
    require((next.models().availability == ToolModelAvailability::available) == usable &&
                (!usable || next.models().revision ==
                                (content ? decode_tool_model_file(*content)
                                               .transform([](const ToolModelFile& f)
                                                              -> std::uint64_t {
                                                 return f.revision;
                                               })
                                               .value_or(0)
                                         : 0)),
            std::format("File {}: a new instance reads what the file holds", name));
  }
  // What this process wrote itself is not an outside change.
  Rig rig{saved(5, held)};
  rig.accepted(assign(5, 9), "A save");
  rig.accepted(assign(6, 10), "and another: the file is what this process wrote");
}

void archive_failures() {
  const ToolModelDocument document{other_defaults(), {{row(4), ToolFamily::chamfer, {}}}};
  const auto good = saved(5, document);
  // Up to and including the rename: the saved file is as it was.
  for (const auto* step : {"open_temporary", "write", "sync", "rename"}) {
    for (const bool with_file : {true, false}) {
      Rig rig{with_file ? std::optional<std::string>{good} : std::nullopt};
      const auto base = rig.revision();
      const auto shown = rig.archive.snapshot();
      rig.files.failures[step] = 1;
      const auto reply = rig.archive.apply(assign(base, 9));
      require(reply.outcome == CommandOutcome::failed && !reply.snapshot &&
                  has(reply.reason, "The tool models were not saved: ") &&
                  has(reply.reason, "The saved file is unchanged."),
              std::format("A failure of {}: failed, the file unchanged: {}", step, reply.reason));
      require((with_file ? rig.disk.files.at(file) == good : !rig.disk.files.contains(file)) &&
                  !rig.disk.files.contains(temporary) && rig.revision() == base &&
                  rig.archive.snapshot() == shown &&
                  // The two of opening, none for a save that did not get that far.
                  rig.files.count("sync_directory") == 2 &&
                  rig.files.count("discard") == 1,
              std::format("A failure of {}: file and snapshot are as before, the temporary file "
                          "is gone: {}",
                          step, steps(rig.files)));
      rig.accepted(assign(base, 9), std::format("The same command again after a failed {}", step));
    }
  }
  // After the rename: uncertain, and the file is read back.
  {
    Rig rig{good};
    rig.files.failures["sync_directory"] = 1;
    const auto calls = rig.files.calls.size();
    const auto reply = rig.archive.apply(assign(5, 9));
    require(reply.outcome == CommandOutcome::failed && reply.snapshot &&
                has(reply.reason, "may not have been saved") && has(reply.reason, "uncertain") &&
                !has(reply.reason, "unchanged"),
            "A failure after the rename is uncertain, not an untouched file: " + reply.reason);
    require(rig.files.calls.back() == "read " + file &&
                rig.files.count("read") == 3 && rig.files.count("discard") == 0,
            "The file is read once more: " + steps(rig.files, calls));
    require(rig.revision() == 6 && rig.available() &&
                doc(rig.archive.models()).assignments.size() == 2 &&
                Json::parse(rig.archive.snapshot()).at("revision") == 6 &&
                rig.on_disk().value_or(ToolModelFile{}).revision == 6,
            "and what it holds is what is shown");
    const auto stale = rig.archive.apply(assign(5, 9));
    require(stale.outcome == CommandOutcome::rejected && stale.snapshot,
            "Trying again from the old revision is stale");
    rig.accepted(assign(6, 10), "Trying again from the snapshot");
  }
  {
    Rig rig{good};
    rig.files.failures["sync_directory"] = 1;
    // The read at commit succeeds, the read after the failure does not.
    int reads = 0;
    rig.files.during = [&](std::string_view step) -> void {
      if (step == "read" && ++reads == 2)
        rig.files.failures["read"] = 1;
    };
    const auto reply = rig.archive.apply(assign(5, 9));
    require(reply.outcome == CommandOutcome::failed && reply.snapshot && !rig.available() &&
                has(rig.archive.models().reason, "cannot be read") &&
                Json::parse(rig.archive.snapshot()).at("availability") == "unavailable",
            "When the file cannot be read back either, nothing is claimed about it");
  }
}

void archive_limits() {
  // 1001 assignments.
  ToolModelDocument full{factory_tool_defaults(), {}};
  for (int number = 1; number <= 1001; ++number)
    full.assignments.push_back({row(number), ToolFamily::generic, {}});
  {
    Rig rig{saved(1, full)};
    const auto before = rig.disk.files.at(file);
    const auto reply = rig.archive.apply(assign(1, 2000));
    require(reply.outcome == CommandOutcome::rejected && !reply.snapshot &&
                has(reply.reason, "At most 1001 tools") && rig.disk.files.at(file) == before &&
                rig.files.count("open_temporary") == 0,
            "A 1002nd assignment is rejected and nothing is written");
    rig.accepted(assign(1, 500, ToolFamily::drill), "Replacing one of 1001");
    rig.accepted(remove(2, 1001), "Removing one of 1001");
    rig.accepted(assign(3, 2000), "The 1001st again");
    require(doc(rig.archive.models()).assignments.size() == 1001, "There are 1001");
  }
  // 1 MiB: every assignment here has the same size, so the count that fits is computed.
  const auto sized = [](std::size_t count) -> ToolModelDocument {
    ToolModelDocument document{factory_tool_defaults(), {}};
    for (std::size_t i = 0; i < count; ++i)
      document.assignments.push_back({{1000 + static_cast<int>(i), 7, 6.5, std::string(255, '\x01')},
                                      ToolFamily::generic, {}});
    return document;
  };
  const auto message_size = [&](std::size_t count) -> std::size_t {
    return encode_tool_models({machine, 2, ToolModelAvailability::available, "", sized(count)})
        .value_or("")
        .size();
  };
  const auto each = message_size(11) - message_size(10);
  const auto fits = 10 + (max_tool_model_bytes - message_size(10)) / each;
  require(each > 1500 && fits < 1001 && message_size(fits) <= max_tool_model_bytes &&
              message_size(fits) + each > max_tool_model_bytes && message_size(fits + 1) == 0,
          std::format("{} assignments of {} bytes fill 1 MiB", fits, each));
  {
    Rig rig{saved(1, sized(fits))};
    require(rig.available(), "A document just below 1 MiB is read");
    const auto before = rig.disk.files.at(file);
    auto over = SetToolModelAssignment{machine, 1, {5, 7, 6.5, std::string(255, '\x01')},
                                       ToolFamily::generic, {}};
    const auto reply = rig.archive.apply(over);
    require(reply.outcome == CommandOutcome::rejected && !reply.snapshot &&
                has(reply.reason, "larger than 1 MiB") && has(reply.reason, "Nothing was saved") &&
                rig.disk.files.at(file) == before && rig.files.count("open_temporary") == 0,
            "A change that would pass 1 MiB is rejected and nothing is written: " + reply.reason);
    over.tool.number = 1000;
    over.tool.comment = "short";
    rig.accepted(over, "A change that makes it smaller");
    require(rig.disk.files.at(file).size() <= max_tool_model_bytes &&
                rig.archive.snapshot().size() <= max_tool_model_bytes,
            "File and snapshot stay within 1 MiB");
  }
  // The last revision.
  {
    Rig rig{saved(last_revision - 1, sized(1))};
    rig.accepted(assign(last_revision - 1, 1), "The save to revision 2^53 - 1");
    const auto before = rig.disk.files.at(file);
    const auto reply = rig.archive.apply(assign(last_revision, 2));
    require(reply.outcome == CommandOutcome::rejected && has(reply.reason, "last revision") &&
                rig.disk.files.at(file) == before && rig.revision() == last_revision,
            "At revision 2^53 - 1 nothing more is saved; the revision does not wrap");
  }
}

void archive_machines() {
  FakeDisk disk;
  FakeToolModelFiles first_files{disk};
  FakeToolModelFiles second_files{disk};
  ToolModelArchive first{first_files, location("mill-1")};
  ToolModelArchive second{second_files, location("mill-2")};
  first.open();
  second.open();
  require(first.apply(assign(0, 1)).outcome == CommandOutcome::accepted &&
              first.apply(defaults(1, other_defaults())).outcome == CommandOutcome::accepted,
          "Machine mill-1 saves");
  require(second.models().revision == 0 && second.apply(remove(0, 1)).snapshot &&
              doc(second.models()).defaults == factory_tool_defaults() &&
              doc(second.models()).assignments.empty() && second.models().revision == 1,
          "Machine mill-2 under the same root sees none of it");
  require(disk.files.contains("/data/mill-1/tool-models.json") &&
              disk.files.contains("/data/mill-2/tool-models.json") &&
              Json::parse(disk.files.at("/data/mill-2/tool-models.json")).at("machine_id") ==
                  "mill-2" &&
              first.models().revision == 2,
          "Each has its directory and its file");

  // A second process on the same machine.
  FakeToolModelFiles other_files{disk};
  ToolModelArchive other{other_files, location("mill-1")};
  other.open();
  const auto before = disk.files.at(file);
  require(other.models().availability == ToolModelAvailability::unavailable &&
              other.models().machine_id == "mill-1" &&
              has(other.models().reason,
                  "/data/mill-1 are already in use by another CoffeeCNC") &&
              other.apply(assign(2, 5)).outcome == CommandOutcome::rejected &&
              other_files.count("read") == 0 && disk.files.at(file) == before,
          "A second process is unavailable, in use; it reads and writes nothing: " +
              other.models().reason);
  require(
      first.apply(SetToolModelAssignment{machine, 2, row(6), ToolFamily::end_mill, {}, "tts-er32"})
              .outcome == CommandOutcome::accepted,
      "The first carries on and saves a holder");

  // Restart: what the old process saved is what the new one shows.
  const auto shown = first.models();
  first.close();
  FakeToolModelFiles next_files{disk};
  ToolModelArchive next{next_files, location("mill-1")};
  next.open();
  require(next.models() == shown && next.models().revision == 3 &&
              doc(next.models()).assignments.back().holder == "tts-er32" &&
              next.snapshot() == first.snapshot(),
          "After a restart the snapshot is the one last saved, byte for byte");
}

// What a store delivered, in order.
struct Delivered {
  std::mutex mutex;
  std::condition_variable arrived;
  std::vector<std::pair<std::uint64_t, std::vector<Json>>> batches;

  auto deliver() -> ToolModelStore::Deliver {
    return [this](PageToken page, const std::vector<std::string>& messages) -> void {
      std::vector<Json> parsed;
      std::ranges::transform(messages, std::back_inserter(parsed),
                             [](const std::string& text) -> Json { return Json::parse(text); });
      {
        const std::scoped_lock lock{mutex};
        batches.emplace_back(page.value, std::move(parsed));
      }
      arrived.notify_all();
    };
  }
  // Waits until `count` deliveries have been made.
  auto wait(std::size_t count) -> std::vector<std::pair<std::uint64_t, std::vector<Json>>> {
    std::unique_lock lock{mutex};
    const bool done = arrived.wait_for(lock, 30s, [&]() -> bool { return batches.size() >= count; });
    require(done, std::format("{} deliveries arrive ({} did)", count, batches.size()));
    return batches;
  }
  auto size() -> std::size_t {
    const std::scoped_lock lock{mutex};
    return batches.size();
  }
};

// Holds the writer inside a step until released.
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{};
  bool open{};
  void pass() {
    std::unique_lock lock{mutex};
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&]() -> bool { return open; });
  }
  void wait_entered() {
    std::unique_lock lock{mutex};
    const bool done = changed.wait_for(lock, 30s, [&]() -> bool { return entered; });
    require(done, "The writer reaches the step it is held in");
  }
  void release() {
    {
      const std::scoped_lock lock{mutex};
      open = true;
    }
    changed.notify_all();
  }
};

auto is_snapshot(const Json& message, std::uint64_t revision) -> bool {
  return message.at("type") == "tool_models" && message.at("revision") == revision;
}
auto is_result(const Json& message, RequestId id, std::string_view outcome) -> bool {
  return message.at("type") == "result" && message.at("id") == id &&
         message.at("outcome") == outcome;
}

void store_orders_messages() {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  Delivered delivered;
  {
    ToolModelStore store{files, location(), delivered.deliver()};
    store.send_snapshot(PageToken{1});
    auto got = delivered.wait(1);
    require(got.at(0).first == 1 && got.at(0).second.size() == 1 &&
                is_snapshot(got.at(0).second.at(0), 0) &&
                got.at(0).second.at(0).at("availability") == "available",
            "The snapshot asked for after hello");
    // Answered at once, without the writer.
    require(
        store.submit(PageToken{1}, 1, assign(0, 1, ToolFamily::generic, {}, "mill-2"))
                .value_or(LocalizedReason{})
                .text.starts_with("This change is for another machine than this one (mill-1)") &&
            store.submit(PageToken{1}, 2, assign(0, 1, ToolFamily::generic, {}, "Mill-1"))
                .has_value(),
        "A command for another machine ID is refused at once, also one differing by case");
    auto bad = other_defaults();
    bad.at(at(ToolFamily::drill)).tip_angle = 180;
    const auto invalid = store.submit(PageToken{1}, 3, defaults(0, bad));
    require(invalid && invalid->text == std::string("drill: ") + angle_reason &&
                invalid->localization ==
                    MessageKey{"backend.toolModels.familyProblem",
                               {{"family", "drill"}, {"detail", angle_reason}},
                               parameter_localization(
                                   "detail", MessageKey{"backend.toolModels.angleRange"})} &&
                store.submit(PageToken{1}, 4, SetToolModelAssignment{machine, 0, row(0), {}, {}})
                    .has_value(),
            "An invalid command is refused at once");
    require(delivered.size() == 1, "and the writer sends nothing for those");

    require(!store.submit(PageToken{1}, 5, defaults(0, other_defaults())), "A valid command waits");
    got = delivered.wait(2);
    require(got.at(1).first == 1 && got.at(1).second.size() == 2 &&
                is_snapshot(got.at(1).second.at(0), 1) &&
                is_result(got.at(1).second.at(1), 5, "accepted") &&
                got.at(1).second.at(1).at("reason") == "",
            "An accepted save: the new snapshot, then its result, in one delivery");
    require(!store.submit(PageToken{1}, 6, assign(0, 1)), "A stale command waits too");
    got = delivered.wait(3);
    require(got.at(2).second.size() == 2 && is_snapshot(got.at(2).second.at(0), 1) &&
                is_result(got.at(2).second.at(1), 6, "rejected") &&
                got.at(2).second.at(1).at("localization").at("key") ==
                    "backend.toolModelStore.revisionChanged" &&
                got.at(2).second.at(1).at("localization").at("params") ==
                    Json{{"shown", "0"}, {"current", "1"}},
            "A stale base revision: the current snapshot, then the rejection");
    files.failures["rename"] = 1;
    require(!store.submit(PageToken{1}, 7, assign(1, 1)), "A command whose write fails");
    got = delivered.wait(4);
    require(got.at(3).second.size() == 1 && is_result(got.at(3).second.at(0), 7, "failed") &&
                got.at(3).second.at(0).at("localization").at("key") ==
                    "backend.toolModelStore.notSavedReason",
            "A failed write: the result alone, the snapshot shown still holds");
    files.failures["sync_directory"] = 1;
    require(!store.submit(PageToken{1}, 8, assign(1, 1)), "A command whose directory fsync fails");
    got = delivered.wait(5);
    require(got.at(4).second.size() == 2 && is_snapshot(got.at(4).second.at(0), 2) &&
                is_result(got.at(4).second.at(1), 8, "failed") &&
                has(got.at(4).second.at(1).at("reason").get<std::string>(), "uncertain"),
            "An uncertain write: the snapshot read back, then the failure");
  }
  require(disk.locked.empty() && files.calls.back() == "unlock",
          "The store releases the lock when it ends");

  // Storage ruled out by the configuration never reaches the writer.
  for (const auto& settings :
       {ToolModelSettings{{}, {}, {}, {}}, ToolModelSettings{"mill-1", "relative", {}, {}},
        ToolModelSettings{"bad id", "/data", {}, {}}}) {
    FakeDisk none;
    FakeToolModelFiles untouched{none};
    Delivered told;
    const auto where = resolve_tool_model_location(settings);
    ToolModelStore store{untouched, where, told.deliver()};
    require(store.submit(PageToken{1}, 1, assign(0, 1)).value_or(LocalizedReason{}).text ==
                    where.reason &&
                store.submit(PageToken{1}, 2, defaults(0, other_defaults()))
                        .value_or(LocalizedReason{})
                        .text == where.reason,
            "Session-only or unavailable storage rejects with its reason, at once");
    store.send_snapshot(PageToken{1});
    const auto got = told.wait(1);
    require(got.at(0).second.at(0).at("availability") ==
                (settings.machine_id ? "unavailable" : "session_only") &&
                got.at(0).second.at(0).at("reason") == where.reason &&
                got.at(0).second.at(0).at("document").is_null() &&
                got.at(0).second.at(0).at("revision") == 0 &&
                got.at(0).second.at(0).at("machine_id") ==
                    (where.machine_id ? Json(*where.machine_id) : Json(nullptr)),
            "and its snapshot says why");
    require(where.localization && got.at(0).second.size() == 2 &&
                got.at(0).second.at(0).size() == 6 &&
                got.at(0).second.at(1).at("type") == "tool_models_reason" &&
                got.at(0).second.at(1).at("reason") == where.reason &&
                got.at(0).second.at(1).at("localization").at("key") == where.localization->key,
            "An unchanged legacy snapshot precedes its localized reason");
  }
  // A lock held elsewhere is found by the writer: its result comes from there.
  {
    FakeDisk shared;
    shared.locked.insert(directory);
    FakeToolModelFiles second{shared};
    Delivered told;
    ToolModelStore store{second, location(), told.deliver()};
    require(!store.submit(PageToken{1}, 1, assign(0, 1)), "The command is taken");
    const auto got = told.wait(1);
    require(got.at(0).second.size() == 1 && is_result(got.at(0).second.at(0), 1, "rejected") &&
                has(got.at(0).second.at(0).at("reason").get<std::string>(), "already in use"),
            "and rejected by the writer: in use");
  }
}

void store_bounds_and_races() {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  Delivered delivered;
  Gate gate;
  std::atomic<bool> hold{true};
  files.during = [&](std::string_view step) -> void {
    if (step == "open_temporary" && hold.exchange(false))
      gate.pass();
  };
  ToolModelStore store{files, location(), delivered.deliver()};
  // All of these were made from the same snapshot, revision 0.
  require(!store.submit(PageToken{1}, 1, assign(0, 1)), "The first command is taken");
  gate.wait_entered();
  for (RequestId id = 2; id <= 9; ++id)
    require(!store.submit(PageToken{1}, id, assign(0, static_cast<int>(id))),
            "Eight commands wait behind the one being written");
  static_assert(ToolModelStore::max_pending == 8, "The bound this test fills");
  require(store.submit(PageToken{1}, 10, assign(0, 10)).value_or(LocalizedReason{}).text ==
                  ToolModelStore::busy_reason &&
              store.submit(PageToken{1}, 11, defaults(0, other_defaults()))
                      .value_or(LocalizedReason{})
                      .text == ToolModelStore::busy_reason,
          "A ninth is rejected at once: the queue is full");
  require(delivered.size() == 0, "Nothing is answered before a write is durable");
  gate.release();
  const auto got = delivered.wait(9);
  require(got.at(0).second.size() == 2 && is_snapshot(got.at(0).second.at(0), 1) &&
              is_result(got.at(0).second.at(1), 1, "accepted"),
          "The first of the racing commands is saved");
  for (std::size_t i = 1; i < 9; ++i)
    require(got.at(i).second.size() == 2 && is_snapshot(got.at(i).second.at(0), 1) &&
                is_result(got.at(i).second.at(1), static_cast<RequestId>(i + 1), "rejected"),
            "Every other one finds revision 1 at its commit: snapshot, then rejected, in order");
  require(files.count("rename") == 1 &&
              decode_tool_model_file(disk.files.at(file))
                      .transform([](const ToolModelFile& f) -> std::size_t {
                        return f.document.assignments.size();
                      }) == 1,
          "One write happened");
  require(!store.submit(PageToken{1}, 12, assign(1, 2)), "The queue has room again");
  static_cast<void>(delivered.wait(10));
}

void store_pages() {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  Delivered delivered;
  Gate gate;
  std::atomic<bool> hold{true};
  files.during = [&](std::string_view step) -> void {
    if (step == "rename" && hold.exchange(false))
      gate.pass();
  };
  {
    ToolModelStore store{files, location(), delivered.deliver()};
    require(!store.submit(PageToken{1}, 1, assign(0, 1)) &&
                !store.submit(PageToken{1}, 2, assign(1, 2)),
            "Two commands of page 1");
    gate.wait_entered();
    // The page is replaced while the first is being written and the second waits.
    store.page_ended(PageToken{1});
    store.send_snapshot(PageToken{1});
    require(!store.submit(PageToken{2}, 1, assign(1, 3)), "A command of page 2 waits");
    store.send_snapshot(PageToken{2});
    gate.release();
    const auto got = delivered.wait(2);
    require(got.at(0).first == 2 && got.at(0).second.size() == 1 &&
                is_snapshot(got.at(0).second.at(0), 1) &&
                got.at(0).second.at(0).at("document").at("assignments").size() == 1 &&
                got.at(0).second.at(0).at("document").at("assignments")[0].at("tool").at(
                    "number") == 1,
            "The write that had begun was finished: the new page's snapshot shows it");
    require(got.at(1).first == 2 && got.at(1).second.size() == 2 &&
                is_snapshot(got.at(1).second.at(0), 2) &&
                is_result(got.at(1).second.at(1), 1, "accepted"),
            "and the new page's command builds on it");
    store.send_snapshot(PageToken{2});
    const auto all = delivered.wait(3);
    require(all.size() == 3 && all.at(2).first == 2,
            "Nothing was delivered for the page that ended: no result, no snapshot");
    const auto saved_now = decode_tool_model_file(disk.files.at(file));
    require(saved_now && saved_now->revision == 2 &&
                saved_now->document.assignments.size() == 2 &&
                saved_now->document.assignments.back().tool.number == 3 &&
                files.count("rename") == 2,
            "The command that waited for the ended page was dropped, not written");
    // An ended page, and those before it, stay ended.
    store.page_ended(PageToken{3});
    store.send_snapshot(PageToken{2});
    store.send_snapshot(PageToken{3});
    require(!store.submit(PageToken{4}, 1, assign(2, 4)), "A command of page 4");
    const auto last = delivered.wait(4);
    require(last.size() == 4 && last.at(3).first == 4 &&
                is_result(last.at(3).second.back(), 1, "accepted"),
            "Snapshots asked for ended pages are not sent");
  }
  // The end of the store: the write in progress is finished, what waits is dropped.
  FakeDisk other;
  FakeToolModelFiles other_files{other};
  Delivered told;
  Gate last;
  std::atomic<bool> once{true};
  other_files.during = [&](std::string_view step) -> void {
    if (step == "write" && once.exchange(false))
      last.pass();
  };
  std::thread releaser;
  {
    ToolModelStore store{other_files, location(), told.deliver()};
    require(!store.submit(PageToken{1}, 1, assign(0, 1)) &&
                !store.submit(PageToken{1}, 2, assign(1, 2)),
            "Two commands, the first held in its write");
    last.wait_entered();
    releaser = std::thread{[&]() -> void {
      std::this_thread::sleep_for(50ms);
      last.release();
    }};
  }
  releaser.join();
  const auto end = decode_tool_model_file(other.files.at(file));
  require(end && end->revision == 1 && end->document.assignments.size() == 1 &&
              !other.files.contains(temporary) && other.locked.empty() &&
              other_files.count("rename") == 1,
          "At the end the write in progress is complete and the waiting one was not begun");
}

auto mode(const std::filesystem::path& path) -> unsigned {
  struct stat info {};
  require(::stat(path.c_str(), &info) == 0, "stat " + path.string());
  return static_cast<unsigned>(info.st_mode) & 07777U;
}

auto contents(const std::filesystem::path& path) -> std::string {
  return read_text_file(path.string(), 2 * max_tool_model_bytes).value_or(std::string());
}

// The real file system and the real lock.
void on_the_host(const std::filesystem::path& scratch) {
  std::filesystem::remove_all(scratch);
  std::filesystem::create_directories(scratch);
  const auto root = scratch / "data root";
  const auto where = resolve_tool_model_location({"Mill_A", root.string(), {}, {}});
  const auto folder = root / "Mill_A";
  const auto path = folder / "tool-models.json";
  {
    LocalToolModelFiles files;
    ToolModelArchive archive{files, where};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::available &&
                archive.models().revision == 0,
            "A directory that did not exist: available, revision 0: " + archive.models().reason);
    require(mode(root) == 0700 && mode(folder) == 0700 && mode(folder / "lock") == 0600,
            "The directories are made 0700, the lock file 0600");
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator{folder})
      names.push_back(entry.path().filename().string());
    require(names == std::vector<std::string>{"lock"},
            "Reading makes no document: the directory holds the lock file alone");

    // A second instance, as a second process has it: its own open file of the same lock.
    LocalToolModelFiles second_files;
    ToolModelArchive second{second_files, where};
    second.open();
    require(second.models().availability == ToolModelAvailability::unavailable &&
                has(second.models().reason, "already in use by another CoffeeCNC") &&
                has(second.models().reason, folder.string()),
            "While the lock is held a second instance is unavailable: " + second.models().reason);
    require(second.apply(assign(0, 1, ToolFamily::generic, {}, "Mill_A")).outcome ==
                CommandOutcome::rejected &&
                !std::filesystem::exists(path),
            "and writes nothing");

    ToolOverrides overrides;
    overrides.stickout = 0.1 + 0.2;
    auto reply = archive.apply(defaults(0, other_defaults(), "Mill_A"));
    require(reply.outcome == CommandOutcome::accepted, "The first save: " + reply.reason);
    reply = archive.apply(assign(1, 3, ToolFamily::drill, overrides, "Mill_A"));
    require(reply.outcome == CommandOutcome::accepted && archive.models().revision == 2,
            "The second save: " + reply.reason);
    require(mode(path) == 0600 && !std::filesystem::exists(folder / "tool-models.json.tmp"),
            "The saved file is 0600 and no temporary file is left");
    const auto read = decode_tool_model_file(contents(path));
    require(read && read->machine_id == "Mill_A" && read->revision == 2 &&
                read->document == archive.models().document,
            "The file holds what the snapshot shows");

    // Still locked for the second, whatever was saved.
    LocalToolModelFiles third_files;
    ToolModelArchive third{third_files, where};
    third.open();
    require(third.models().availability == ToolModelAvailability::unavailable,
            "The lock is held for as long as the first lives");
    const auto shown = archive.snapshot();
    archive.close();

    // Restart.
    LocalToolModelFiles next_files;
    ToolModelArchive next{next_files, where};
    next.open();
    require(next.models().availability == ToolModelAvailability::available &&
                next.snapshot() == shown && next.models().revision == 2 &&
                bits(doc(next.models()).assignments.front().overrides.stickout.value_or(0)) ==
                    bits(0.1 + 0.2),
            "After a restart the snapshot is the saved one, numbers bit for bit");
    // Another machine under the same root.
    LocalToolModelFiles other_files;
    ToolModelArchive other{other_files,
                           resolve_tool_model_location({"mill_a", root.string(), {}, {}})};
    other.open();
    require(other.models().availability == ToolModelAvailability::available &&
                other.models().revision == 0 && std::filesystem::exists(root / "mill_a" / "lock") &&
                !std::filesystem::exists(root / "mill_a" / "tool-models.json"),
            "Machine mill_a under the same root has nothing of Mill_A's");
  }
  // Whatever the umask: directories 0700 and files 0600 exactly. A machine directory that was
  // there and is open to others is used and reported.
  for (const mode_t mask : {mode_t{0}, mode_t{0277}, mode_t{022}}) {
    const auto before = ::umask(mask);
    const auto base = scratch / std::format("umask-{:o}", static_cast<unsigned>(mask));
    const auto place = resolve_tool_model_location({"m", (base / "a" / "b").string(), {}, {}});
    LocalToolModelFiles files;
    ToolModelArchive archive{files, place};
    archive.open();
    const auto reply = archive.apply(SetToolModelAssignment{"m", 0, row(1), ToolFamily::generic, {}});
    static_cast<void>(::umask(before));
    require(reply.outcome == CommandOutcome::accepted && archive.models().reason.empty(),
            std::format("With umask {:o} a save succeeds: {}{}", static_cast<unsigned>(mask),
                        reply.reason, archive.models().reason));
    require(mode(base) == 0700 && mode(base / "a") == 0700 && mode(base / "a" / "b") == 0700 &&
                mode(base / "a" / "b" / "m") == 0700 && mode(base / "a" / "b" / "m" / "lock") == 0600 &&
                mode(base / "a" / "b" / "m" / "tool-models.json") == 0600,
            std::format("With umask {:o}: every directory made is 0700, every file 0600",
                        static_cast<unsigned>(mask)));
  }
  {
    const auto wide = scratch / "wide";
    std::filesystem::create_directories(wide / "m");
    require(::chmod((wide / "m").c_str(), 0755) == 0, "The test directory gets mode 0755");
    LocalToolModelFiles files;
    ToolModelArchive archive{files, resolve_tool_model_location({"m", wide.string(), {}, {}})};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::available &&
                has(archive.models().reason, "can be entered by other users (mode 755)") &&
                mode(wide / "m") == 0755,
            "A machine directory that was there with mode 0755 is used, left as it is, and "
            "reported: " + archive.models().reason);
  }
  // The store, with its thread, on the real files.
  {
    LocalToolModelFiles files;
    Delivered delivered;
    ToolModelStore store{files, where, delivered.deliver()};
    store.send_snapshot(PageToken{1});
    require(!store.submit(PageToken{1}, 1,
                          SetToolModelAssignment{"Mill_A", 2, row(3), std::nullopt, {}})
                 .has_value(),
            "A removal is taken");
    const auto got = delivered.wait(2);
    require(is_snapshot(got.at(0).second.at(0), 2) && is_snapshot(got.at(1).second.at(0), 3) &&
                is_result(got.at(1).second.at(1), 1, "accepted") &&
                got.at(1).second.at(0).at("document").at("assignments").empty(),
            "The store saves through the real files");
  }
  // A corrupt file is left exactly as it is.
  {
    std::ofstream{path, std::ios::binary | std::ios::trunc} << "{\"machine_id\":";
    LocalToolModelFiles files;
    ToolModelArchive archive{files, where};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::unavailable &&
                has(archive.models().reason, path.string()) &&
                archive.apply(assign(0, 1, ToolFamily::generic, {}, "Mill_A")).outcome ==
                    CommandOutcome::rejected &&
                contents(path) == "{\"machine_id\":",
            "A corrupt file on the host: unavailable and untouched: " + archive.models().reason);
  }
  // A directory where the settings file should be, and a root that cannot be made.
  {
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    LocalToolModelFiles files;
    ToolModelArchive archive{files, where};
    archive.open();
    require(archive.models().availability == ToolModelAvailability::unavailable &&
                has(archive.models().reason, "cannot be read"),
            "What is not a regular file cannot be read: " + archive.models().reason);
    const auto state = Json::parse(encode_tool_models_reason(archive.models()).value());
    require(state.at("localization").at("params_localization").at("detail").at("key") ==
                "backend.toolModelStore.notRegularFile",
            "The native read failure retains its storage detail key");
    LocalToolModelFiles blocked_files;
    ToolModelArchive blocked{blocked_files,
                             resolve_tool_model_location(
                                 {"m", (folder / "lock" / "below").string(), {}, {}})};
    blocked.open();
    require(blocked.models().availability == ToolModelAvailability::unavailable &&
                has(blocked.models().reason, "cannot be opened"),
            "A directory that cannot be made: unavailable: " + blocked.models().reason);
  }
  std::filesystem::remove_all(scratch);
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: coffeecnc_tool_model_tests <scratch directory>");
    rules();
    locations();
    wire_text();
    file_refusals();
    archive_reads();
    archive_directories();
    archive_writes();
    archive_outside_changes();
    archive_failures();
    archive_limits();
    archive_machines();
    store_orders_messages();
    store_bounds_and_races();
    store_pages();
    on_the_host(std::filesystem::absolute(argv[1]) / "tool_models");
    std::cout << checks << " tool model checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
