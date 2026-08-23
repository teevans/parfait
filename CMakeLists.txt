cmake_minimum_required(VERSION 3.22)
project(parfait VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Quick Qml Network Sql QuickControls2)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(OPUSENC REQUIRED IMPORTED_TARGET libopusenc)

# whisper.cpp is optional at build time: clone into third_party/whisper.cpp to enable.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper.cpp/CMakeLists.txt)
    set(PARFAIT_WITH_WHISPER ON)
    add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)
else()
    set(PARFAIT_WITH_WHISPER OFF)
    message(STATUS "third_party/whisper.cpp not found — building with stub transcriber")
endif()

file(GLOB_RECURSE PARFAIT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
file(GLOB_RECURSE PARFAIT_QML CONFIGURE_DEPENDS qml/*.qml)

qt_add_executable(parfait ${PARFAIT_SOURCES})

# Strip the qml/ prefix so files land at qrc:/Parfait/<File>.qml, matching the URI.
foreach(qml_file IN LISTS PARFAIT_QML)
    file(RELATIVE_PATH qml_rel ${CMAKE_CURRENT_SOURCE_DIR}/qml ${qml_file})
    set_source_files_properties(${qml_file} PROPERTIES QT_RESOURCE_ALIAS ${qml_rel})
endforeach()

qt_add_qml_module(parfait
    URI Parfait
    VERSION 1.0
    QML_FILES ${PARFAIT_QML}
    RESOURCE_PREFIX /
)

target_include_directories(parfait PRIVATE src)
target_link_libraries(parfait PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Sql Qt6::QuickControls2
    PkgConfig::PIPEWIRE PkgConfig::OPUSENC)

if(PARFAIT_WITH_WHISPER)
    target_compile_definitions(parfait PRIVATE PARFAIT_WITH_WHISPER)
    target_link_libraries(parfait PRIVATE whisper)
endif()

install(TARGETS parfait RUNTIME DESTINATION bin)

# Headless end-to-end harness: real AudioEngine -> TranscribeEngine, no GUI.
# Sources are listed explicitly; the GLOB above only covers src/, not tests/.
add_executable(parfait-e2e
    tests/e2e/main.cpp
    src/Types.h                 # Q_GADGET: AUTOMOC needs the header in the sources
    src/audio/AudioEngine.cpp
    src/transcribe/TranscribeEngine.cpp)

target_include_directories(parfait-e2e PRIVATE src)
target_link_libraries(parfait-e2e PRIVATE
    Qt6::Core PkgConfig::PIPEWIRE PkgConfig::OPUSENC)

if(PARFAIT_WITH_WHISPER)
    target_compile_definitions(parfait-e2e PRIVATE PARFAIT_WITH_WHISPER)
    target_link_libraries(parfait-e2e PRIVATE whisper)
endif()
