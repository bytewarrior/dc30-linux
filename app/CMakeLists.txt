# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
project(dc30-capture VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets DBus LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(SWR REQUIRED IMPORTED_TARGET libswresample libavutil)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavformat)

add_executable(dc30-capture
	src/main.cpp
	src/devices.cpp
	src/videocapture.cpp
	src/metacapture.cpp
	src/audiocapture.cpp
	src/alsamixer.cpp
	src/audiomonitor.cpp
	src/recorder.cpp
	src/fieldaligner.cpp
	src/fieldfix.cpp
	src/fieldrepair.cpp
	src/levelmeter.cpp
	src/previewwidget.cpp
	src/mainwindow.cpp
	src/aboutdialog.cpp
	src/colorscheme.cpp
)
target_compile_definitions(dc30-capture PRIVATE APP_VERSION="${PROJECT_VERSION}")
# Window icon and the about box's logo, under :/
qt_add_resources(dc30-capture images PREFIX "/" FILES app_icon.png app_logo.png)
# dc30_meta.h, shared with the driver
target_include_directories(dc30-capture PRIVATE ${CMAKE_SOURCE_DIR}/../src)
target_compile_options(dc30-capture PRIVATE -Wall -Wextra)
target_link_libraries(dc30-capture PRIVATE Qt6::Widgets Qt6::DBus PkgConfig::ALSA PkgConfig::SWR
	PkgConfig::AV)

# German translation, embedded under :/i18n; English is the source language.
# Refresh after changing strings: cmake --build build --target update_translations
qt_add_translations(dc30-capture TS_FILES i18n/dc30-capture_de.ts)

# Installation: binary, desktop entry, icon (cmake --install build)
include(GNUInstallDirs)
install(TARGETS dc30-capture RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES dc30-capture.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES app_icon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps
	RENAME dc30-capture.png)

# Tests: ctest --test-dir build
enable_testing()
add_executable(fieldaligner_test tests/fieldaligner_test.cpp src/fieldaligner.cpp)
target_include_directories(fieldaligner_test PRIVATE src)
target_compile_options(fieldaligner_test PRIVATE -Wall -Wextra)
add_test(NAME fieldaligner COMMAND fieldaligner_test)
