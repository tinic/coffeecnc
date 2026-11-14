#ifndef COFFEECNC_APP_SESSION_HPP
#define COFFEECNC_APP_SESSION_HPP

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
  // Protocol v1 JSON for the current page, in order.
  std::vector<std::string> messages;
  // The page sent `ready`: send hello, the tool table, the program and status.
  bool send_snapshots{};
};

// The single owner of request handling: page generations, the ready gate, request identity,
// policy, mode switching, controller actions and jog leases. Every request that is executed or
// rejected gets exactly one CommandResult; stale, duplicate and replayed requests get none.
//
// Pure logic: no threads, no I/O, time passed in. NOT thread-safe: every member must be called
// from one thread. The port blocks in a live build, so a later increment moves it to a worker.
class Session {
public:
  // The port must outlive the session. Throws std::invalid_argument for invalid limits.
  Session(ControllerPort& port, const Limits& limits);

  // Call on every page load. Stops held jogs and forgets the previous page's ready and ID state;
  // messages carrying an earlier token are dropped from here on.
  [[nodiscard]] auto begin_page() -> PageToken;

  [[nodiscard]] auto handle(PageToken page, std::string_view json, Instant now) -> Outcome;

  // Call from the control loop, well inside JogLease::timeout. Stops jogs whose lease expired,
  // and every held jog once the controller link is not live or the machine is not on.
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
  [[nodiscard]] auto mdi(const std::string& text) -> Reply;
  void stop(const JogLease::Stops& stops);
  void stop_if_held(Axis axis);
  void cancel_jogs();
  void report(Outcome& outcome, Instant now);

  ControllerPort& port_;
  CommandPolicy policy_;
  JogLease leases_;
  // Meaningful only while the axis has an active lease.
  std::array<HeldJog, 3> held_{};
  std::uint64_t pages_{};
  std::optional<RequestId> last_id_;
  bool ready_{};
  // Failed stops that are not the result of a request; sent as operator messages.
  std::vector<std::string> problems_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_SESSION_HPP
