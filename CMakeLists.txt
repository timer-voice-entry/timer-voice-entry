cmake_minimum_required(VERSION 3.24)
project(voice_entry_time LANGUAGES CXX)

# Objective-C++ for the macOS app. CMake supports OBJCXX on every generator,
# Ninja included, so this costs no second build system.
if(APPLE)
  enable_language(OBJCXX)
endif()

# The C++ standard is set per target, NOT globally. A global CMAKE_CXX_STANDARD
# leaks into every FetchContent subproject, and some of sherpa-onnx's transitive
# dependencies still use std::result_of, which C++20 removed. Let them pick their
# own; our targets ask for 20 via target_compile_features.
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Off by default: whisper.cpp and sherpa-onnx dominate build time, and only the
# `voice` command needs them. Turn on with -DTT_ENABLE_VOICE=ON.
option(TT_ENABLE_VOICE "Build the speech pipeline (whisper.cpp + sherpa-onnx)" OFF)

include(cmake/Dependencies.cmake)

# tt_core -- domain model and its SQLite persistence. Sources are added here as
# each layer lands.
add_library(tt_core
  src/compliance.cpp
  src/db.cpp
  src/match.cpp
  src/timeutil.cpp
  src/tracker.cpp
  src/tracker_write.cpp
)
target_include_directories(tt_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tt_core PUBLIC SQLiteCpp nlohmann_json::nlohmann_json)
target_compile_features(tt_core PUBLIC cxx_std_20)
target_compile_options(tt_core PRIVATE -Wall -Wextra -Wpedantic)

# tt_agent -- the conversation with the model, and the tool contract that lets it
# reach tt_core. httplib is PRIVATE: it appears in agent.cpp and nowhere else, so
# nothing that links tt_agent inherits an HTTP dependency.
add_library(tt_agent
  src/agent.cpp
)
target_link_libraries(tt_agent PUBLIC tt_core PRIVATE httplib::httplib)
target_compile_features(tt_agent PUBLIC cxx_std_20)
target_compile_options(tt_agent PRIVATE -Wall -Wextra -Wpedantic)

# tt -- the one executable. voice.cpp is compiled straight in rather than made a
# library, because this is its only consumer. With TT_ENABLE_VOICE off, `tt voice`
# still exists and reports that speech was not built.
add_executable(tt apps/tt/main.cpp)
target_link_libraries(tt PRIVATE tt_agent)
target_compile_options(tt PRIVATE -Wall -Wextra -Wpedantic)

if(TT_ENABLE_VOICE)
  target_sources(tt PRIVATE src/voice.cpp)
  # miniaudio is a vendored header with no target of its own. whisper and
  # sherpa-onnx-c-api carry their own include paths, ggml's among them.
  target_include_directories(tt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  target_link_libraries(tt PRIVATE whisper sherpa-onnx-c-api)
  target_compile_definitions(tt PRIVATE TT_VOICE=1)
endif()

# ---------------------------------------------------------------------------
# TimeTracker.app -- the macOS application. Objective-C++, so it calls tt_agent
# and tt_core directly; there is no wrapper layer.
# ---------------------------------------------------------------------------
if(APPLE AND TT_ENABLE_VOICE)
  add_executable(TimeTracker MACOSX_BUNDLE
    apps/mac/main.mm
    src/voice.cpp
  )
  target_link_libraries(TimeTracker PRIVATE tt_agent whisper sherpa-onnx-c-api)
  target_include_directories(TimeTracker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  # ARC: the app stores autoreleased objects in ivars (_groups, _query). Without
  # it those are freed when the pool drains and read again on the next draw.
  # ObjC++ under ARC still handles the std::unique_ptr members fine.
  target_compile_options(TimeTracker PRIVATE -Wall -Wextra -fobjc-arc)
  target_link_options(TimeTracker PRIVATE -framework Cocoa)

  set_target_properties(TimeTracker PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME           "TimeTracker"
    MACOSX_BUNDLE_GUI_IDENTIFIER        "com.robertpratl.timetracker"
    MACOSX_BUNDLE_BUNDLE_VERSION        "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING  "0.1"
    MACOSX_BUNDLE_INFO_PLIST            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
  )

  # The app chdir's to Resources at launch, so models/ and prompts/ resolve the
  # same way they do from the terminal.
  set(TT_RES "$<TARGET_BUNDLE_CONTENT_DIR:TimeTracker>/Resources")
  add_custom_command(TARGET TimeTracker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/prompts" "${TT_RES}/prompts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/models"  "${TT_RES}/models"
    # The guidelines the preflight agent reads. Without this check_rules throws
    # on first use and the answer comes back with no citation.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"    "${TT_RES}/data"
    # Ad-hoc sign with a stable identifier. Without this the bundle gets a fresh
    # TCC identity on every build and macOS re-asks for the microphone each time.
    COMMAND codesign --force --sign - --identifier "com.robertpratl.timetracker"
            "$<TARGET_BUNDLE_DIR:TimeTracker>"
    VERBATIM
  )
endif()
