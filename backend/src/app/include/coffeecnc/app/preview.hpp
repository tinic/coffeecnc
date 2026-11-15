#ifndef COFFEECNC_APP_PREVIEW_HPP
#define COFFEECNC_APP_PREVIEW_HPP

#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/core/preview.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace coffeecnc {

// Where the composer gets previews from. It hands over the inputs of every new context and
// takes whatever is finished; it never waits. Thread-safe.
class PreviewService {
public:
  PreviewService() = default;
  virtual ~PreviewService() = default;

  // The inputs of a new context, with a program loaded. The newest context wins: a computation
  // for an older one is given up.
  virtual void submit(std::uint64_t context, const PreviewInputs& inputs) = 0;
  // The preview finished last, of whatever context; null before the first.
  [[nodiscard]] virtual auto latest() const -> std::shared_ptr<const Preview> = 0;
  // Counts the times a file of the latest preview was found changed. It is a preview input
  // (PreviewInputs::files_generation): the context goes up with it, the preview shown is then
  // stale by the protocol's own rule, and the new context is submitted like any other.
  [[nodiscard]] virtual auto files_generation() const -> std::uint64_t = 0;

protected:
  PreviewService(const PreviewService&) = default;
  PreviewService(PreviewService&&) = default;
  auto operator=(const PreviewService&) -> PreviewService& = default;
  auto operator=(PreviewService&&) -> PreviewService& = default;
};

// The preview thread: computes with the engine, one computation at a time, always for the
// context submitted last.
//
//   thread   period            owns                         may block on
//   preview  on a new context, the PreviewEngine and what   the engine: its helper process,
//            and every         it returned; every read of   the file system
//            check_period      a program or subroutine
//            while a preview   file for a preview
//            is current
//
// While a preview is current the thread asks every `check_period` whether its files changed
// (PreviewComputation::files_changed); a change raises files_generation, once per preview.
// A computation and a look at the files are both given up for a newer context and for the
// end of the worker; how soon is the engine's matter (HelperPreviewEngine: within one file
// read or comparison, or 20 ms while its helper runs).
// Nothing here is called by the GTK, control, status or command thread but submit, latest and
// files_generation, which return at once.
class PreviewWorker final : public PreviewService {
public:
  static constexpr auto default_check_period = std::chrono::milliseconds{1000};

  // The engine must outlive the worker. Starts the thread.
  explicit PreviewWorker(PreviewEngine& engine,
                         std::chrono::milliseconds check_period = default_check_period);
  // Gives up the computation in progress and joins the thread.
  ~PreviewWorker() override;
  PreviewWorker(const PreviewWorker&) = delete;
  auto operator=(const PreviewWorker&) -> PreviewWorker& = delete;

  void submit(std::uint64_t context, const PreviewInputs& inputs) override;
  [[nodiscard]] auto latest() const -> std::shared_ptr<const Preview> override;
  [[nodiscard]] auto files_generation() const -> std::uint64_t override;

private:
  struct Request {
    std::uint64_t context{};
    PreviewInputs inputs;
  };
  void loop();

  PreviewEngine& engine_;
  std::chrono::milliseconds check_period_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<Request> wanted_;
  std::shared_ptr<const Preview> latest_;
  // The context submitted last: a computation for another one is of no use.
  std::atomic<std::uint64_t> newest_{};
  std::atomic<std::uint64_t> generation_{};
  std::atomic<bool> waiting_{}; // a request waits in wanted_
  std::atomic<bool> stop_{};
  std::thread thread_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_PREVIEW_HPP
