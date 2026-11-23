find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)

add_library(coffeecnc_app src/alarm_log.cpp src/file_system.cpp src/messages.cpp src/offset_history.cpp src/panel_assets.cpp src/panel_feed.cpp src/preview.cpp src/program_edit.cpp src/program_progress.cpp src/program_store.cpp src/request.cpp
  src/response.cpp src/runtime.cpp src/session.cpp src/status_publisher.cpp src/tool_model_store.cpp
  src/tool_models.cpp src/operator_settings.cpp src/operator_settings_store.cpp)
target_include_directories(coffeecnc_app PUBLIC include)
target_compile_features(coffeecnc_app PUBLIC cxx_std_23)
target_link_libraries(coffeecnc_app PUBLIC coffeecnc_core Threads::Threads
  PRIVATE coffeecnc_project_options nlohmann_json::nlohmann_json)
coffeecnc_enable_analyzers(coffeecnc_app)
