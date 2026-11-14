#ifndef COFFEECNC_APP_SESSION_HPP
#define COFFEECNC_APP_SESSION_HPP

#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/request.hpp>
#include <coffeecnc/app/response.hpp>
#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/core/jog_lease.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// One page load in the native shell. Never on the wire. A default token matches no page.
struct PageToken {
  std::uint64_t value{};
  friend auto operator==(const PageToken&, const PageToken&) -> bool = default;
};

// The caller's clocks, read once per call.
struct Instant {
  JogLease::Time monotonic{};            // jog leases
  std::chrono::milliseconds unix_time{}; // stamps operator messages
};

// What one call asks of the caller.
struct Outcome {
  // The page the messages are for. The caller posts them only while this is still the page it
  // shows: replies queued across a reload must not reach the next page.
  PageToken page;
  // Protocol v1 JSON for that page, in order. A `directory` precedes the result of the
  // `list_directory` that asked for it; when that listing is truncated, an `info` message saying
  // so follows the result.
  std::vector<std::string> messages;
  // The page sent `ready`: send hello, the tool table, the program and status.
  bool send_snapshots{};
};

// The single owner of request handling: page generations, the ready gate, request identity,
// policy, mode switching, controller actions, the program directory and jog leases. Every request
// that is executed or rejected gets exactly one CommandResult; stale, duplicate and replayed
// requests get none.
//
// `list_directory` is answered from the program store, not the controller. `open_program` reaches
// the controller only with a path the store resolved: a regular file inside the program
// directory; anything else is `rejected`.
//
// Pure logic: no threads, no I/O, time passed in. NOT thread-safe: every member must be called
// from one thread. The port blocks in a live build, so a later increment moves it to a worker.
class Session {
public:
  // The port and the store must outlive the session. Throws std::invalid_argument for invalid
  // limits.
  Session(ControllerPort& port, const Limits& limits, const ProgramStore& programs);
  // Without a program store: `list_directory` is rejected and `open_program` passes its path to
  // the port UNCHECKED. Only for the LinuxCNC simulation tests, which write their own program.
  Session(ControllerPort& port, const Limits& limits);

  // Call on every page load. Stops held jogs and forgets the previous page's ready and ID state;
  // messages carrying an earlier token are dropped from here on.
  [[nodiscard]] auto begin_page() -> PageToken;

  // Call when the page unloads or the window closes without a replacement. Stops held jogs;
  // every message is dropped until the next begin_page. Ignored for a token that is not current.
  void end_page(PageToken page);

  [[nodiscard]] auto handle(PageToken page, std::string_view json, Instant now) -> Outcome;

  // Call from the control loop, well inside JogLease::timeout. Stops jogs whose lease expired,
  // every held jog once the controller link is not live or the machine is not on, and each held
  // jog the policy would no longer start in the observed state (mode, interpreter, homing or
  // jog space changed outside the panel). Retries every stop that failed earlier.
  // The port blocks in a live build: before live use the lease/stop path must be serviced
  // independently of a port call in progress (worker increment).
  [[nodiscard]] auto tick(Instant now) -> Outcome;

private:
  struct Reply {
    CommandOutcome outcome{CommandOutcome::accepted};
    std::string reason;
  };
  struct HeldJog {
    int direction{};
    double velocity{};
    JogSpace space{};
    friend auto operator==(const HeldJog&, const HeldJog&) -> bool = default;
  };

  [[nodiscard]] static auto reply(ActionResult result) -> Reply;
  [[nodiscard]] auto in_order(RequestId id) -> bool;
  [[nodiscard]] auto execute(const Command& command, JogLease::Time now) -> Reply;
  [[nodiscard]] auto perform(const Command& command, const Decision& decision,
                             const ControllerState& state, JogLease::Time now) -> Reply;
  [[nodiscard]] auto jog(const Jog& request, const Decision& decision, JogLease::Time now)
      -> ActionResult;
  [[nodiscard]] auto stop_all() -> ActionResult;
  [[nodiscard]] auto tracked_stop(std::size_t index) -> ActionResult;
  [[nodiscard]] auto mdi(const std::string& text) -> Reply;
  [[nodiscard]] auto list_directory(const ListDirectory& request) -> Reply;
  void stop(const JogLease::Stops& stops);
  void stop_if_held(Axis axis);
  void cancel_jogs();
  void report(Outcome& outcome, Instant now);

  ControllerPort& port_;
  const ProgramStore* programs_{};
  CommandPolicy policy_;
  JogLease leases_;
  // Meaningful only while the axis has an active lease.
  std::array<HeldJog, 3> held_{};
  // Axes whose last stop failed. Not a lease: nothing renews it, and tick retries the stop
  // until the controller accepts one.
  JogLease::Stops stop_pending_{};
  std::uint64_t pages_{};
  bool page_open_{};
  std::optional<RequestId> last_id_;
  bool ready_{};
  // Failed stops that are not the result of a request; sent as operator messages.
  std::vector<std::string> problems_;
  // Messages the request in hand produced besides its result: sent before it.
  std::vector<std::string> produced_;
  // For the operator, about a request that was accepted: sent after its result.
  std::vector<std::string> notices_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_SESSION_HPP
