cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32_C3_OLED)

add_custom_target(bump-version
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/main/version_bump.py patch
    COMMENT "Bump patch version"
    VERBATIM
)

add_custom_target(bump-version-minor
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/main/version_bump.py minor
    COMMENT "Bump minor version"
    VERBATIM
)

add_custom_target(bump-version-major
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/main/version_bump.py major
    COMMENT "Bump major version"
    VERBATIM
)