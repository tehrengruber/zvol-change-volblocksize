cmake_minimum_required(VERSION 3.16)
project(zvol-change-volblocksize VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

# Shared POSIX process helpers, used by the tool and the tests.
add_library(zcvb_common STATIC src/subprocess.cpp)
target_include_directories(zcvb_common PUBLIC src)

add_executable(zvol-change-volblocksize src/main.cpp)
target_link_libraries(zvol-change-volblocksize PRIVATE zcvb_common)

# Optional fully-static build: a single dependency-free binary that needs no
# installation.  Best paired with a musl toolchain (e.g. an Alpine container) for
# a truly portable result; also works with a glibc toolchain that ships static
# libs.  The tool touches no NSS/locale APIs, so static linking is safe here.
option(ZCVB_STATIC "Link the tool as a fully static, dependency-free binary" OFF)
if(ZCVB_STATIC)
  target_link_options(zvol-change-volblocksize PRIVATE -static)
endif()

install(TARGETS zvol-change-volblocksize RUNTIME DESTINATION bin)

include(CTest)  # defines BUILD_TESTING (default ON) and calls enable_testing()
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# --- Debian packaging: `cpack -G DEB` produces a .deb --------------------- #
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "zvol-change-volblocksize")
set(CPACK_PACKAGE_CONTACT "Till Ehrengruber <till@ehrengruber.ch>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Change a ZFS zvol's volblocksize while retaining all snapshots")
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "zfsutils-linux")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)  # auto libc/libstdc++ dependencies
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
