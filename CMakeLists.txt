# SPDX-License-Identifier: MIT
# Home Automation panel for Waveshare ESP32-S3-Touch-LCD-3.5-C
# Requires ESP-IDF v5.3.5
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(home_automation_panel)
