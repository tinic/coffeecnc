// Which files the panel's URL scheme serves, on a directory the test makes.
//   coffeecnc_panel_assets_tests <scratch directory>

#include <coffeecnc/app/panel_assets.hpp>

#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace coffeecnc;

namespace {

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

void write(const std::filesystem::path& file) {
  std::filesystem::create_directories(file.parent_path());
  std::ofstream out{file};
  out << "x";
}

void run(const std::filesystem::path& scratch) {
  const auto base = scratch / "panel_assets";
  std::filesystem::remove_all(base);
  const auto root = base / "ui";
  write(root / "index.html");
  write(root / "assets" / "index-abc.js");
  write(root / "assets" / "occt-import-js-abc.wasm");
  write(root / "assets" / "with space.css");
  write(root / "fonts" / "Inter.woff2");
  write(base / "secret.txt");
  std::filesystem::create_symlink(base / "secret.txt", root / "assets" / "escape.txt");
  std::filesystem::create_symlink(root / "index.html", root / "assets" / "inside.html");
  const auto canonical = std::filesystem::canonical(root);

  const auto status = [&](std::string_view url) -> int {
    const auto asset = resolve_asset(root.string(), url);
    return asset ? 200 : asset.error();
  };
  const auto file = [&](std::string_view url) -> std::string {
    const auto asset = resolve_asset(root.string(), url);
    return asset ? asset->file : std::string();
  };
  const auto type = [&](std::string_view url) -> std::string_view {
    const auto asset = resolve_asset(root.string(), url);
    return asset ? asset->content_type : std::string_view{};
  };

  require(file("coffeecnc://panel/index.html") == (canonical / "index.html").string() &&
              type("coffeecnc://panel/index.html") == "text/html",
          "The entry page is served as text/html");
  require(file("coffeecnc://panel/") == (canonical / "index.html").string() &&
              file("coffeecnc://panel") == (canonical / "index.html").string(),
          "The origin itself is the entry page");
  require(type("coffeecnc://panel/assets/index-abc.js") == "text/javascript" &&
              type("coffeecnc://panel/assets/occt-import-js-abc.wasm") == "application/wasm" &&
              type("coffeecnc://panel/fonts/Inter.woff2") == "font/woff2",
          "Scripts, WebAssembly and fonts have their exact media types, without parameters");
  require(status("coffeecnc://panel/assets/index-abc.js?v=1#top") == 200,
          "Query and fragment are ignored");
  require(file("coffeecnc://panel/assets/with%20space.css") ==
              (canonical / "assets" / "with space.css").string(),
          "Percent escapes are resolved");
  require(status("coffeecnc://panel/assets/inside.html") == 200,
          "A link that stays inside the directory is served");

  require(status("coffeecnc://panel/assets/missing.js") == 404 &&
              status("coffeecnc://panel/missing/page") == 404,
          "A missing file is 404, never the entry page");
  require(status("coffeecnc://panel/assets") == 404 && status("coffeecnc://panel/assets/") == 404,
          "A directory is not listed");
  require(status("coffeecnc://panel/../secret.txt") == 403 &&
              status("coffeecnc://panel/assets/../../secret.txt") == 403 &&
              status("coffeecnc://panel/%2e%2e/secret.txt") == 403 &&
              status("coffeecnc://panel/assets/%2E%2E%2F%2E%2E%2Fsecret.txt") == 403 &&
              status("coffeecnc://panel/./index.html") == 403,
          "Path traversal is refused, escaped or not");
  require(status("coffeecnc://panel//etc/passwd") == 403 &&
              status("coffeecnc://panel/%2Fetc%2Fpasswd") == 403,
          "An absolute path is refused");
  require(status("coffeecnc://panel/assets%5Cindex-abc.js") == 403 &&
              status("coffeecnc://panel/index.html%00.js") == 403 &&
              status("coffeecnc://panel/%zz") == 403 && status("coffeecnc://panel/%2") == 403,
          "Backslashes, control characters and malformed escapes are refused");
  require(status("coffeecnc://panel/assets/escape.txt") == 403,
          "A link that leaves the directory is refused");
  require(status("coffeecnc://other/index.html") == 404 &&
              status("coffeecnc://panel:80/index.html") == 404 &&
              status("coffeecnc://user@panel/index.html") == 404 &&
              status("coffeecnc://panelx/index.html") == 404 &&
              status("coffeecnc://PANEL/index.html") == 404 &&
              status("coffeecnc:///index.html") == 404 &&
              status("http://panel/index.html") == 404 && status("file:///etc/passwd") == 404,
          "Another host or scheme is not the panel");
  require(status("coffeecnc://panel/index.html") == 200 &&
              !resolve_asset((base / "nowhere").string(), "coffeecnc://panel/index.html"),
          "A panel directory that does not exist serves nothing");

  require(is_panel_url("coffeecnc://panel/index.html?tab=program") &&
              is_panel_url("coffeecnc://panel") && !is_panel_url("coffeecnc://other/") &&
              !is_panel_url("https://example.org/") && !is_panel_url("about:blank") &&
              !is_panel_url("coffeecnc://panel.example/"),
          "Only the panel's origin is a place to navigate to");
  require(content_type_of("a/b.c/readme") == "application/octet-stream" &&
              content_type_of("X.SVG") == "image/svg+xml" &&
              content_type_of("part.step") == "model/step" &&
              content_type_of("data.bin") == "application/octet-stream",
          "Unknown extensions are plain bytes; the extension's case does not matter");

  // The STEP model beside a program: same directory, same base name, .step or .stp.
  const auto programs = base / "programs";
  write(programs / "plate.ngc");
  write(programs / "plate.STEP");
  write(programs / "lone.ngc");
  write(programs / "other.step");
  const auto part_url = [](const std::filesystem::path& program) {
    std::string url = "coffeecnc://panel/program-part?program=";
    for (const char c : program.string())
      url += c == ' ' ? std::string{"%20"} : std::string(1, c);
    return url;
  };
  const auto plate = resolve_program_part(part_url(programs / "plate.ngc"));
  require(plate && std::filesystem::path(*plate).filename() == "plate.STEP",
          "A program's model is found beside it, whatever the extension's case");
  require(!resolve_program_part(part_url(programs / "lone.ngc")),
          "A program without a model of its name has none");
  require(!resolve_program_part(part_url(programs / "missing.ngc")),
          "A program that does not exist has no model");
  require(!resolve_program_part("coffeecnc://panel/program-part?program=plate.ngc") &&
              !resolve_program_part("coffeecnc://panel/program-part") &&
              !resolve_program_part("coffeecnc://panel/index.html?program=" +
                                    (programs / "plate.ngc").string()),
          "Only an absolute program path on the program-part URL names a model");
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: coffeecnc_panel_assets_tests <scratch directory>");
    run(argv[1]);
    std::cout << checks << " panel asset checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
