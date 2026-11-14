// coffeecnc: the operator panel as a DISPLAY program of LinuxCNC. See application.hpp for the
// start-up and shutdown order.

#include "application.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

auto main(int argc, char** argv) -> int {
  try {
    const std::span<char*> raw{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string_view> arguments{raw.begin() + (raw.empty() ? 0 : 1), raw.end()};
    const auto options = coffeecnc::parse_options(arguments);
    if (!options) {
      std::cerr << "coffeecnc: " << options.error() << "\n\n" << coffeecnc::usage();
      return EXIT_FAILURE;
    }
    if (options->help) {
      std::cout << coffeecnc::usage();
      return EXIT_SUCCESS;
    }
    coffeecnc::configure_environment();
    return coffeecnc::run_application(*options);
  } catch (const std::exception& error) {
    std::cerr << "coffeecnc: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
