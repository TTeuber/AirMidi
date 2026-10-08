cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The version string comes from `git describe` of this repo (ESP-IDF does that when PROJECT_VER is not set),
# so a build is traceable to its commit; release builds are tagged vX.Y.Z.
project(airmidi)
