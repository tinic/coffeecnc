// coffeecnc: the operator panel as a DISPLAY program of LinuxCNC. See application.hpp for the
// start-up and shutdown order.

#include "application.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

auto main(int argc, char** argv) -> int {
  try {
    // Without the program name.
    const std::vector<std::string_view> arguments(argv + std::min(argc, 1), argv + argc);
    const auto options = coffeecnc::parse_options(arguments);
    if (!options) {
      std::cerr << "coffeecnc: " << options.error() << "\n\n" << coffeecnc::usage();
      return EXIT_FAILURE;
    }
    if (options->help) {
      std::cout << coffeecnc::usage();
      return EXIT_SUCCESS;
    }
    if (options->check)
      return coffeecnc::check_configuration(*options);
    coffeecnc::configure_environment();
    return coffeecnc::run_application(*options);
  } catch (const std::exception& error) {
    std::cerr << "coffeecnc: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
