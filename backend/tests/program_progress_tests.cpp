#include <coffeecnc/app/messages.hpp>
#include <coffeecnc/app/program_progress.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace coffeecnc;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

void require(bool condition, const char* reason) {
  if (!condition)
    throw std::runtime_error(reason);
}

auto reading(const fs::path& file) -> PanelReading {
  PanelReading result;
  auto& status = result.status;
  status.link = Link::live;
  status.machine = MachineState::on;
  status.mode = TaskMode::automatic;
  status.tool.in_spindle = 98;
  status.motion.in_position = true;
  status.program.file = file.string();
  status.program.revision = 7;
  result.program = std::make_shared<const ProgramText>(
      ProgramText{7, file.string(), {"G21", "G0 X0", "G0 Z10", "G1 Y10 F60", "M30"}, false, "0"});
  return result;
}

void moves_on_line(PanelReading& sample, int line) {
  sample.status.interp = InterpState::reading;
  sample.status.program.current_line = 5; // lookahead already reached M30
  sample.status.program.motion_line = line;
  sample.status.motion.in_position = false;
  sample.task_executing = true;
}

void at_rest(PanelReading& sample) {
  sample.status.interp = InterpState::idle;
  sample.status.motion.in_position = true;
  sample.task_executing = false;
}

void stops_and_reloads(const fs::path& directory) {
  const auto part = directory / "part.ngc";
  std::ofstream(part) << "G21\nG0 X0\nG0 Z10\nG1 Y10 F60\nM30\n";
  auto sample = reading(part);
  const auto now = ProgramProgress::Clock::now();
  LocalToolModelFiles files;
  ProgramProgress progress{files, directory.string()};
  progress.observe(sample, now);
  require(!sample.status.program.restart_hint && sample.status.program.cycle == ProgramCycle::idle,
          "An untouched program has no restart hint or completed cycle.");
  moves_on_line(sample, 3);
  progress.observe(sample, now + 100ms);
  require(!sample.status.program.restart_hint && sample.status.program.cycle == ProgramCycle::running,
          "A cutting cycle is running before a restart hint exists.");
  moves_on_line(sample, 4);
  progress.observe(sample, now + 200ms);
  // A subroutine can move on an earlier source line after the caller's first motion.
  moves_on_line(sample, 2);
  progress.observe(sample, now + 225ms);
  ++sample.program_aborts;
  progress.observe(sample, now + 250ms);
  at_rest(sample);
  progress.observe(sample, now + 300ms);
  require(sample.status.program.restart_hint == RestartHint{2, 98} &&
              sample.status.program.cycle == ProgramCycle::idle,
          "Stop saves the block in motion, ignoring interpreter lookahead.");

  LocalToolModelFiles reloaded_files;
  ProgramProgress reloaded{reloaded_files, directory.string()};
  auto reopened = reading(part);
  reloaded.observe(reopened, now + 350ms);
  require(reopened.status.program.restart_hint == RestartHint{2, 98},
          "A new session restores the stopped line for the same file.");
  const auto status = encode_status(reopened.status, 1);
  require(status && nlohmann::json::parse(*status).at("program").at("restart_hint") ==
                        nlohmann::json{{"line", 2}, {"tool", 98}},
          "The status publishes both fields expected by the restart button.");
  auto other = reading(directory / "different.ngc");
  std::ofstream(other.program->file) << "G21\nM30\n";
  reloaded.observe(other, now + 400ms);
  require(!other.status.program.restart_hint, "Another file cannot inherit the restart hint.");

  moves_on_line(reopened, 4);
  reloaded.observe(reopened, now + 450ms);
  at_rest(reopened);
  reloaded.observe(reopened, now + 500ms);
  require(!reopened.status.program.restart_hint && reopened.status.program.cycle == ProgramCycle::completed,
          "A completed run confirms the completed cycle and removes the saved hint.");
  moves_on_line(reopened, 3);
  reloaded.observe(reopened, now + 2s);
  // A process or power loss may occur while moving; the record is already on disk.
  ProgramProgress power_loss{files, directory.string()};
  auto after_power_loss = reading(part);
  power_loss.observe(after_power_loss, now + 2500ms);
  require(after_power_loss.status.program.restart_hint == RestartHint{3, 98},
          "A session lost during motion leaves a durable restart hint.");

  moves_on_line(reopened, 4);
  reloaded.observe(reopened, now + 2600ms);
  reopened.status.machine = MachineState::estop;
  at_rest(reopened);
  reloaded.observe(reopened, now + 2700ms);
  require(reopened.status.program.restart_hint == RestartHint{4, 98},
          "E-stop preserves the most recent block and forces a save.");
  fs::last_write_time(part, fs::last_write_time(part) + 2s);
  reloaded.observe(reopened, now + 2800ms);
  require(!reopened.status.program.restart_hint, "An edited file has no stale restart hint.");

  auto partial = reading(part);
  ProgramText prefix = *partial.program;
  prefix.truncated = true;
  prefix.lines.resize(2);
  partial.program = std::make_shared<const ProgramText>(std::move(prefix));
  moves_on_line(partial, 4);
  reloaded.observe(partial, now + 2900ms);
  require(partial.status.program.cycle == ProgramCycle::running, "Partial program text can still time a cut.");
  at_rest(partial);
  reloaded.observe(partial, now + 3s);
  require(partial.status.program.cycle == ProgramCycle::completed && !partial.status.program.restart_hint,
          "A partial text snapshot does not establish a restart line.");
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 2)
      throw std::runtime_error("Expected a test data directory.");
    const fs::path directory{argv[1]};
    fs::create_directories(directory);
    fs::remove(directory / "program-progress.json");
    stops_and_reloads(directory);
    std::cout << "Program progress checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
