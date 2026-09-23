# SPDX-License-Identifier: MIT
# Home Automation panel for Waveshare ESP32-S3-Touch-LCD-3.5-C
# Requires ESP-IDF v5.3.5
#
# This project builds fully offline. Every third-party dependency is vendored
# under third_party/, and there are no idf_component.yml manifests, so the
# IDF component manager never contacts components.espressif.com.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components"
    "${CMAKE_CURRENT_LIST_DIR}/third_party")

# Belt and braces: even with no manifests present, this stops the component
# manager from running at all.
set(ENV{IDF_COMPONENT_MANAGER} "0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(home_automation_panel)
