find_package(nlohmann_json 3.11 REQUIRED)

add_library(coffeecnc_app src/request.cpp)
target_include_directories(coffeecnc_app PUBLIC include)
target_compile_features(coffeecnc_app PUBLIC cxx_std_23)
target_link_libraries(coffeecnc_app PUBLIC coffeecnc_core
  PRIVATE coffeecnc_project_options nlohmann_json::nlohmann_json)
coffeecnc_enable_analyzers(coffeecnc_app)
