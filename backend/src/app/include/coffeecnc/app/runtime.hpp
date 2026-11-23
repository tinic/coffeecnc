#ifndef COFFEECNC_APP_RUNTIME_HPP
#define COFFEECNC_APP_RUNTIME_HPP

#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/session.hpp>
#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/core/safety_outputs.hpp>
#include <coffeecnc/core/status_source.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
#include <thread>

namespace coffeecnc {

// One control period on the calling thread: heartbeat first, then the dead-man decision, then the
// jog-stop level. Nothing in it waits for the controller. Returns the level asked for.
auto control_step(Session& session, SafetyOutputs& outputs, Instant now) -> bool;

// One status reading on the calling thread, handed to the session.
void status_step(Session& session, StatusSource& source);

// The session on its three threads.
//
//   thread    period   owns                         may block on
//   control   10 ms    SafetyOutputs (heartbeat,    the session mutex only
//                      jog-stop level), dead-man
//   status    5 ms     StatusSource                 the controller's status buffer
//   worker    on work  ControllerPort (commands,    the controller, up to the port's bounds
//                      error channel)
//
// The control period is 10 ms: a tenth of the 150 ms jog lease and a thirtieth of the 300 ms
// watchdog timeout, twice the 5 ms the jog-stop level is kept at least, and at 3000 mm/min an
// axis travels 0.5 mm in it.
//
// begin_page, end_page and submit are called by the shell's thread, one thread at a time, and
// return without waiting for the controller. What cannot be answered at once arrives at the sink,
// called on the worker thread only; it must not block and must not call the runtime.
class Runtime {
public:
  using Sink = std::function<void(const Outcome&)>;
  // Called on the worker thread after the sink has the reply to an accepted `quit`.
  using Quit = std::function<void(PageToken)>;
  static constexpr auto control_period = std::chrono::milliseconds{10};
  static constexpr auto status_period = std::chrono::milliseconds{5};

  struct Timing {
    std::uint64_t control_periods{};
    // Longest time from the start of one control period to the start of the next.
    std::chrono::nanoseconds longest_control_period{};
  };

  // Everything passed in must outlive the runtime; the tool-model service may be null. Starts
  // the threads. Throws std::invalid_argument for invalid limits.
  Runtime(ControllerPort& port, StatusSource& status, SafetyOutputs& outputs, const Limits& limits,
          const ProgramStore& programs, Sink sink, ToolModelService* tool_models = nullptr,
          Quit quit = {}, PreviewWorker* preview = nullptr, PreviewEngine* restart_engine = nullptr,
          ProgramWriter* program_writer = nullptr, OffsetHistoryStore* history = nullptr,
          DiagnosticsService* diagnostics = nullptr,
          ProgramWriter* copy_writer = nullptr);
  // Shutdown. From the first control period after it begins, at most control_period later, the
  // jog-stop level is high, with or without a jog, and nothing lowers it again. The worker
  // finishes the call it is in, executes waiting stop requests and stops each axis that may be
  // moving, once; the status thread finishes its reading; the control thread beats and holds
  // the level all that time, writes the level high once more and ends last. The destructor
  // joins the threads: it returns when the port and status calls in progress have returned, which
  // is bounded by their own bounds (see NmlPort) and not by this class. It never abandons a
  // thread that is inside LinuxCNC's libraries. The heartbeat stops with the control thread: the
  // watchdog then inhibits jogs as well.
  ~Runtime();
  Runtime(const Runtime&) = delete;
  auto operator=(const Runtime&) -> Runtime& = delete;

  [[nodiscard]] auto begin_page(bool watch_frames = false) -> PageToken;
  void end_page(PageToken page);
  void panel_frame(PageToken page, std::chrono::milliseconds rendered_at);
  // What is answered at once; see Session::submit.
  [[nodiscard]] auto submit(PageToken page, std::string_view json,
                            std::chrono::milliseconds rendered_at = {}) -> Outcome;

  // Session::loaded_program.
  [[nodiscard]] auto loaded_program() const -> std::string;

  // Waits on the disk worker, never on the network or panel thread.
  [[nodiscard]] auto publish_program(const std::string& path, const Session::Publication& publish)
      -> std::expected<void, std::string>;

  // Since the start or the last call of reset_timing.
  [[nodiscard]] auto timing() const -> Timing;
  void reset_timing();

private:
  void control_loop();
  void status_loop();
  void work_loop();
  void copy_loop(ProgramWriter& writer);

  StatusSource& status_;
  SafetyOutputs& outputs_;
  Sink sink_;
  Quit quit_;
  Session session_;
  std::atomic<bool> closing_{};
  std::atomic<bool> stop_control_{};
  std::atomic<bool> stop_status_{};
  std::atomic<std::uint64_t> control_periods_{};
  std::atomic<std::chrono::nanoseconds::rep> longest_control_period_{};
  std::thread control_;
  std::thread status_thread_;
  std::thread worker_;
  std::thread copier_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_RUNTIME_HPP
