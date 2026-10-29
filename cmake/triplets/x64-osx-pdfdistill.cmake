set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 11.0)
set(VCPKG_BUILD_TYPE release)
# Recent Apple clang searches /usr/local/include before -isystem directories;
# make sure ports see vcpkg's headers (e.g. libjpeg-turbo) before any stray
# Homebrew/MacPorts copies there.
if(DEFINED CURRENT_INSTALLED_DIR)
  set(VCPKG_C_FLAGS "-I${CURRENT_INSTALLED_DIR}/include")
  set(VCPKG_CXX_FLAGS "-I${CURRENT_INSTALLED_DIR}/include")
endif()
