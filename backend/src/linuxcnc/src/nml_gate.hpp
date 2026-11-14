#ifndef COFFEECNC_LINUXCNC_SRC_NML_GATE_HPP
#define COFFEECNC_LINUXCNC_SRC_NML_GATE_HPP

#include <memory>
#include <mutex>
#include <utility>

namespace coffeecnc {

// The one lock around LinuxCNC's NML library. libnml is not thread-safe and separate channel
// objects do not make it so: it shares state between all of them, and not only on error paths.
// In LinuxCNC 2.9.4 (paths below src/libnml):
//
//   buffer/shmem.cc:44-57  not_zero() writes the file-static last_x and last_non_zero_x;
//                          main_access() (line 442) calls it in every read, peek and write
//   nml/nml.cc:48          NML_Main_Channel_List, changed by every constructor and destructor
//   cms/cms.cc:52          CMS::number_of_cms_objects
//   rcs/rcs_print.cc:38-52 the print list, the last-error buffers and their index, the error
//                          counter: written by every error and debug print
//
// So every entry into libnml, without exception, is one call made through this gate: the
// constructors and destructors of the channels (they read the NML configuration file and print),
// valid, peek, read, write, get_address, and with them the formatter and every print they call.
// A channel is reachable only through Gated, which leaves no other way to call it.
//
// The gate is held for one libnml call and never across a sleep or a poll loop: a thread that
// waits for LinuxCNC between calls does not keep the others out. One call can last as long as
// libnml waits for the buffer's System V semaphore, which a stopped task may hold: the timeout
// of the `xemc` lines in the NML configuration file, 10 s in LinuxCNC's linuxcnc.nml. That is
// the longest the gate is held, and it delays every other thread that wants NML by as much. The
// control thread makes no NML call and never takes the gate.
[[nodiscard]] auto nml_gate() -> std::mutex&;

// An NML channel that can only be used through the gate.
template <typename Channel> class Gated {
public:
  template <typename... Arguments> explicit Gated(Arguments&&... arguments) {
    const std::scoped_lock lock{nml_gate()};
    channel_ = std::make_unique<Channel>(std::forward<Arguments>(arguments)...);
  }
  ~Gated() {
    const std::scoped_lock lock{nml_gate()};
    channel_.reset();
  }
  Gated(const Gated&) = delete;
  auto operator=(const Gated&) -> Gated& = delete;

  // One libnml call: `use` must make exactly one, and must not wait for anything else.
  template <typename Use> auto call(Use&& use) -> decltype(auto) {
    const std::scoped_lock lock{nml_gate()};
    return std::forward<Use>(use)(*channel_);
  }

private:
  std::unique_ptr<Channel> channel_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_SRC_NML_GATE_HPP
