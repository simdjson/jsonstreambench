# Bootstrap CPM.cmake (CMake Missing Package Manager).
#
# CPM.cmake itself is downloaded into the *build* directory, so no dependency
# bytes ever land in this repository. Set CPM_SOURCE_CACHE (e.g. in your
# environment) to share one checkout of simdjson/Pison across build trees and
# machines:
#
#   export CPM_SOURCE_CACHE=$HOME/.cache/CPM
set(CPM_DOWNLOAD_VERSION 0.43.1)
set(CPM_HASH_SUM
    "1c40fc102ce9625d7de7eb14f541cab30cc3138dca627f0b0ec40293ce6c2934")

if(CPM_SOURCE_CACHE)
  set(CPM_DOWNLOAD_LOCATION
      "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
  set(CPM_DOWNLOAD_LOCATION
      "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
  set(CPM_DOWNLOAD_LOCATION
      "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

if(NOT (EXISTS ${CPM_DOWNLOAD_LOCATION}))
  message(STATUS "Downloading CPM.cmake to ${CPM_DOWNLOAD_LOCATION}")
  file(
    DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
    ${CPM_DOWNLOAD_LOCATION}
    EXPECTED_HASH SHA256=${CPM_HASH_SUM})
endif()

include(${CPM_DOWNLOAD_LOCATION})
