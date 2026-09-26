cmake_minimum_required(VERSION 3.16)
project(tui_agent CXX)

# VERSION at the repo root is the single source of truth for the build version.
# It is injected as a raw, unquoted token and stringized in agent/version.hpp,
# so the same value reaches this file, build.ps1 and build-linux.sh.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" EMBER_VERSION_RAW)
string(STRIP "${EMBER_VERSION_RAW}" EMBER_VERSION)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TUI_WARNINGS -Wall -Wextra)
if(NOT MSVC)
  list(APPEND TUI_WARNINGS -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# TUI library (zero third-party dependency)
# ---------------------------------------------------------------------------
add_library(tui STATIC
  tui/src/platform.cpp
  tui/src/screen.cpp
  tui/src/renderer.cpp
  tui/src/input.cpp
  tui/src/widgets.cpp
  tui/src/md.cpp
  tui/src/form.cpp
  tui/src/drawing.cpp
)
target_include_directories(tui PUBLIC tui/include third_party)
target_compile_options(tui PRIVATE ${TUI_WARNINGS})

# ---------------------------------------------------------------------------
# Agent core library (client + tools + context; no UI)
# ---------------------------------------------------------------------------
add_library(agent_core STATIC
  agent/src/api.cpp
  agent/src/http.cpp
  agent/src/tools.cpp
  agent/src/background.cpp
  agent/src/context.cpp
  agent/src/plugins.cpp
  agent/src/patch.cpp
  agent/src/search.cpp
  agent/src/rag.cpp
  agent/src/session.cpp
  agent/src/settings.cpp
  agent/src/trajectory.cpp
  agent/src/usage.cpp
  agent/src/cache.cpp
)
target_include_directories(agent_core PUBLIC agent/include)
target_compile_options(agent_core PRIVATE ${TUI_WARNINGS})
target_link_libraries(agent_core PUBLIC tui)

# Windows-only by design, and not registered as tools elsewhere (see tools.cpp):
# uia.cpp needs UI Automation, browser.cpp drives Chrome over CDP with Winsock.
if(WIN32)
  target_sources(agent_core PRIVATE agent/src/uia.cpp agent/src/browser.cpp)
endif()

# ---------------------------------------------------------------------------
# Agent executable
# ---------------------------------------------------------------------------
add_executable(agent
  agent/src/main.cpp
  agent/src/app.cpp
)
target_compile_definitions(agent PRIVATE EMBER_VERSION=${EMBER_VERSION})
target_include_directories(agent PRIVATE agent/include)
target_link_libraries(agent PRIVATE agent_core)

if(WIN32)
  target_link_libraries(agent PRIVATE winhttp ws2_32)
  target_link_libraries(agent_core PRIVATE winhttp ws2_32 uiautomationcore ole32 oleaut32 user32 gdi32)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_link_options(agent PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(agent_core PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
else()
  find_package(CURL REQUIRED)
  target_link_libraries(agent PRIVATE CURL::libcurl)
  target_link_libraries(agent_core PRIVATE CURL::libcurl)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
add_executable(demo examples/demo.cpp)
target_include_directories(demo PRIVATE tui/include)
target_link_libraries(demo PRIVATE tui)

add_executable(showcase examples/showcase.cpp)
target_include_directories(showcase PRIVATE tui/include)
target_link_libraries(showcase PRIVATE tui)

add_executable(studio examples/studio.cpp)
target_include_directories(studio PRIVATE tui/include)
target_link_libraries(studio PRIVATE tui)

add_executable(mcp_echo examples/mcp_echo.cpp)
target_include_directories(mcp_echo PRIVATE third_party)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
file(GLOB TEST_SOURCES tests/*.cpp)
foreach(src ${TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  if(NOT WIN32 AND (name STREQUAL "test_browser" OR name STREQUAL "test_desktop"
                    OR name STREQUAL "test_alltools"))
    continue()  # exercise the CDP / UI Automation paths, which are not built here
  endif()
  add_executable(${name} ${src})
  if(name STREQUAL "test_client" OR name STREQUAL "test_tools" OR name STREQUAL "test_context"
     OR name STREQUAL "test_settings" OR name STREQUAL "test_trajectory"
     OR name STREQUAL "test_background" OR name STREQUAL "test_usage"
     OR name STREQUAL "test_plugins" OR name STREQUAL "test_mcp"
     OR name STREQUAL "test_patch" OR name STREQUAL "test_search"
     OR name STREQUAL "test_session" OR name STREQUAL "test_rag"
     OR name STREQUAL "test_browser" OR name STREQUAL "test_alltools")
    target_link_libraries(${name} PRIVATE agent_core)
  else()
    target_link_libraries(${name} PRIVATE tui)
  endif()
  target_compile_options(${name} PRIVATE ${TUI_WARNINGS})
  add_test(NAME ${name} COMMAND ${name})
endforeach()
