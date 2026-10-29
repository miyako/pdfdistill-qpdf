cmake_minimum_required(VERSION 3.21)

# Use the repository's overlay triplets (static, release-only, macOS 11+).
if(NOT DEFINED VCPKG_OVERLAY_TRIPLETS)
  set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/triplets" CACHE STRING "")
endif()
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  # Static CRT (/MT) on Windows.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

project(pdfdistill-qpdf VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Use -I (not -isystem) for dependency headers: recent Apple clang searches
# /usr/local/include before -isystem paths, which can pick up foreign headers.
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(qpdf CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)

# qpdf's exported target links "z;jpeg" by bare name, which would make the
# linker pick up system/Homebrew shared libraries. Point it at the static
# vcpkg libraries instead so the binary stays self-contained.
get_target_property(_qpdf_libs qpdf::libqpdf INTERFACE_LINK_LIBRARIES)
if(_qpdf_libs)
  # Names differ per platform (z/zs/zlib/zlibstatic, jpeg/jpeg-static, ...)
  # and may be wrapped in $<LINK_ONLY:...>.
  set(_mapped "")
  foreach(_lib IN LISTS _qpdf_libs)
    set(_name "${_lib}")
    set(_link_only OFF)
    if(_name MATCHES "^\\$<LINK_ONLY:(.*)>$")
      set(_name "${CMAKE_MATCH_1}")
      set(_link_only ON)
    endif()
    string(REGEX REPLACE "\\.lib$" "" _name "${_name}")
    if(_name MATCHES "^(z|zs|zd|zsd|zlib|zlibd|zlibstatic|zlibstaticd)$")
      set(_name "ZLIB::ZLIB")
    elseif(_name MATCHES "^(jpeg|jpeg-static|jpeg62|turbojpeg|turbojpeg-static)$")
      set(_name "JPEG::JPEG")
    else()
      set(_name "${_lib}")
      set(_link_only OFF)
    endif()
    if(_link_only)
      set(_name "$<LINK_ONLY:${_name}>")
    endif()
    list(APPEND _mapped "${_name}")
  endforeach()
  set(_qpdf_libs "${_mapped}")
  message(STATUS "qpdf::libqpdf link interface: ${_qpdf_libs}")
  set_target_properties(qpdf::libqpdf PROPERTIES INTERFACE_LINK_LIBRARIES "${_qpdf_libs}")
endif()

add_executable(pdfdistill-qpdf
  src/main.cpp
  src/util.cpp
  src/dedup.cpp
  src/content.cpp
  src/fonts.cpp
  src/images.cpp
  src/jpeg.cpp
)
target_compile_definitions(pdfdistill-qpdf PRIVATE PDFDISTILL_VERSION="${PROJECT_VERSION}")
target_link_libraries(pdfdistill-qpdf PRIVATE
  qpdf::libqpdf harfbuzz::harfbuzz-subset JPEG::JPEG ZLIB::ZLIB)
if(MSVC)
  target_compile_options(pdfdistill-qpdf PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pdfdistill-qpdf PRIVATE -Wall -Wextra)
endif()
if(APPLE)
  target_link_options(pdfdistill-qpdf PRIVATE -dead_strip)
endif()

install(TARGETS pdfdistill-qpdf RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
