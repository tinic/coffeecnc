// Bounded reproduction for the unmodified a114848 preview_wire.cpp.
// One MiB per-allocation cap and a one-second alarm prevent runaway allocation.
#include "preview_wire.hpp"
#include <cmath>
#include <iostream>
#include <new>
#include <string>
#include <cstdlib>
#include <unistd.h>
void* operator new(std::size_t bytes) {
  if (bytes > 1024 * 1024) throw std::bad_alloc{};
  if (auto* p = std::malloc(bytes)) return p;
  throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
int main(int argc, char** argv) {
  alarm(1);
  const std::string which = argc > 1 ? argv[1] : "good";
  const std::string text = which == "bad" ? "seg 1 f 0 0 bad 0 1 0 0\n"
    : which == "nan" ? "seg 1 f 0 nan 0 0 1 0 0\nend ok\n"
    : "seg 1 f 0 0 0 0 1 0 0\nend ok\n";
  try {
    auto r = coffeecnc::parse_output(text);
    std::cout << "returned segments=" << r.segments.size()
              << " end=" << static_cast<int>(r.end);
    if (!r.segments.empty())
      std::cout << " first_x_finite=" << std::isfinite(r.segments[0].points[0][0]);
    std::cout << '\n';
    return 0;
  } catch (const std::bad_alloc&) {
    std::cout << "bad_alloc: instrumented 1 MiB per-allocation cap exceeded\n";
    return 42;
  }
}
