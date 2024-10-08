# Copyright (c) Team CharLS.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.9...3.14)

project(charls VERSION 2.1.0 LANGUAGES C CXX)

# Determine if project is built as a subproject (using add_subdirectory) or if it is the master project.
set(MASTER_PROJECT OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MASTER_PROJECT ON)
  message(STATUS "Building as master project, CMake version: ${CMAKE_VERSION}")
endif ()

# The basic options to control what is build extra.
option(CHARLS_BUILD_TESTS "Build test application" ${MASTER_PROJECT})
option(CHARLS_BUILD_SAMPLES "Build sample applications" ${MASTER_PROJECT})
option(CHARLS_INSTALL "Generate the install target." ${MASTER_PROJECT})

# The options used by the CI builds to ensure the source remains warning free.
# Not enabled by default to make CharLS package and end-user friendly.
option(CHARLS_PEDANTIC_WARNINGS "Enable extra warnings and static analysis." OFF)
option(CHARLS_THREAT_WARNINGS_AS_ERRORS "Treat Warnings as Errors." OFF)

# CharLS requires C++14 or newer.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Configure the supported C++ compilers: gcc, clang and MSVC
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(PEDANTIC_CXX_COMPILE_FLAGS
    -pedantic-errors
    -Wall
    -Wextra
    -pedantic
    -Wold-style-cast
    -Wfloat-equal
    -Wlogical-op
    -Wundef
    -Wredundant-decls
    -Wshadow
    -Wwrite-strings
    -Wpointer-arith
    -Wcast-qual
    -Wformat=2
    -Wmissing-include-dirs
    -Wcast-align
    -Wctor-dtor-privacy
    -Wdisabled-optimization
    -Winvalid-pch
    -Woverloaded-virtual
    -Wnon-virtual-dtor
    -Wnoexcept
    -Wdouble-promotion
    -Wtrampolines
    -Wzero-as-null-pointer-constant
    -Wuseless-cast
    -Wvector-operation-performance
    -Wsized-deallocation
  )
  if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 6.0)
      set(PEDANTIC_CXX_COMPILE_FLAGS ${PEDANTIC_CXX_COMPILE_FLAGS}
        -Wshift-overflow=2
        -Wnull-dereference
        -Wduplicated-cond
      )
  endif()

  set(WERROR_FLAG -Werror)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(PEDANTIC_CXX_COMPILE_FLAGS
      -Weverything
      -Wpedantic
      -Wno-weak-vtables                       # Ignore, linker will remove the couple of extra vtables.
      -Wno-padded                             # Ignore, padding optimization is not needed.
      -Wno-c++98-compat                       # Ignore, CharLS 2.x targets C++14, ignore C++98 compatibility.
      -Wno-c++98-compat-pedantic              # Ignore, CharLS 2.x targets C++14, ignore C++98 compatibility.
      -Wno-global-constructors                # Ignore, by design CharLS uses types created at startup.
      -Wno-switch-enum                        # Ignore, cases are handled by default.
      -Wno-sign-conversion                    # Ignore, would just introduce ugly static_asserts.
      -Wno-exit-time-destructors              # Ignore, by design exit-time destructors are used.
      -Wno-missing-braces                     # Ignore, False warning in clang 5.0, fixed in 6.0.
  )

  if (NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.0.2)
    set(PEDANTIC_CXX_COMPILE_FLAGS ${PEDANTIC_CXX_COMPILE_FLAGS}
      -Wno-undefined-func-template            # Ignore, linker will complain if final template code is not available.
    )
  endif()

  set(WERROR_FLAG -Werror)
endif()

if(MSVC)
  set(PEDANTIC_CXX_COMPILE_FLAGS /W4)
  set(WERROR_FLAG /WX)
endif()

# When enabled apply the pedantic warnings options and warnings as errors to globally.
if(CHARLS_PEDANTIC_WARNINGS)

  if(MSVC)
    # Remove existing warning level (W3 is added by default by CMake), duplicate level will generate cmd-line warnings.
    string(REGEX REPLACE " /W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  endif()

  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${PEDANTIC_CXX_COMPILE_FLAGS}>")
endif()

if(CHARLS_THREAT_WARNINGS_AS_ERRORS)
  add_compile_options(${WERROR_FLAG})
endif()

include(src/CMakeLists.txt)

if(CHARLS_BUILD_TESTS)
  add_subdirectory(test)
endif()

if(CHARLS_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()