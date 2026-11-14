find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)

add_library(coffeecnc_app src/file_system.cpp src/messages.cpp src/panel_assets.cpp src/panel_feed.cpp src/program_store.cpp src/request.cpp
  src/response.cpp src/runtime.cpp src/session.cpp src/status_publisher.cpp)
target_include_directories(coffeecnc_app PUBLIC include)
target_compile_features(coffeecnc_app PUBLIC cxx_std_23)
target_link_libraries(coffeecnc_app PUBLIC coffeecnc_core Threads::Threads
  PRIVATE coffeecnc_project_options nlohmann_json::nlohmann_json)
coffeecnc_enable_analyzers(coffeecnc_app)
