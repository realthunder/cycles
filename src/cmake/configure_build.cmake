# Copyright 2011-2020 Blender Foundation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

###########################################################################
# Global generic CMake settings.

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

###########################################################################
# Per-compiler configuration.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_COMPILER_IS_GNUCXX)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wno-sign-compare -fno-strict-aliasing -fPIC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-sign-compare -Wno-invalid-offsetof -fno-strict-aliasing -std=c++17 -fPIC")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wno-sign-compare -fno-strict-aliasing -fPIC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-sign-compare -Wno-invalid-offsetof -fno-strict-aliasing -std=c++17 -fPIC")
endif()

if(APPLE)
  if(NOT ${CMAKE_GENERATOR} MATCHES "Xcode")
    # force CMAKE_OSX_DEPLOYMENT_TARGET for makefiles, will not work else ( cmake bug ? )
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET} -std=c++17 -stdlib=libc++")
    add_definitions("-DMACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()

  # Silence warnings with new linker.
  list(APPEND CMAKE_EXE_LINKER_FLAGS "-Xlinker -no_warn_duplicate_libraries")
  list(APPEND CMAKE_SHARED_LINKER_FLAGS "-Xlinker -no_warn_duplicate_libraries")
elseif(MSVC)
  # NOTE (FreeCAD fork): the else() branch below FORCEs the GLOBAL flag
  # cache variables. That is upstream behaviour -- this file assumes it
  # is the top-level project -- and it is right for a standalone Cycles
  # build. It is wrong for an embedded one: as a subdirectory of a host
  # project it sets the compiler flags for the WHOLE enclosing build.
  # On the FreeCAD tree that cost three things, none of them intended
  # here. CMake's own MSVC defaults (/DWIN32 /D_WINDOWS /W3 /GR) were
  # discarded, so two "#ifdef WIN32" blocks in FreeCAD silently took
  # their non-Windows branch. /J -- plain char UNSIGNED -- was imposed
  # on every translation unit, while the OCCT and Coin libraries whose
  # headers inline into them are built with signed char. And until the
  # /DNDEBUG below, no configuration defined NDEBUG at all, so assert()
  # stayed live against libraries compiled with it. The last two are ODR
  # mismatches, not preferences.
  #
  # So under CYCLES_EMBEDDED the same flags are applied with
  # add_compile_options(), which reaches this directory and the
  # subdirectories added after it, and nothing else. The
  # per-configuration strings are not reproduced: CMake's own MSVC
  # defaults match them term for term but for /MD, which comes from
  # CMAKE_MSVC_RUNTIME_LIBRARY instead, and NDEBUG reaches Cycles' own
  # sources through the per-configuration directory COMPILE_DEFINITIONS
  # property the top-level CMakeLists sets. /std:c++17 is dropped too:
  # CMAKE_CXX_STANDARD 20 above already emits -std:c++20 after it, so on
  # the command line it was only ever dead text.
  #
  # The string(APPEND CMAKE_CXX_FLAGS ...) calls further down need no
  # such treatment -- a set() without CACHE is scoped to this directory
  # already, which is why they never leaked and the ones above did.
  #
  # /J is not carried over even for Cycles itself, and that is deliberate
  # rather than an omission. Scoping it here would only move the char
  # mismatch rather than remove it: the host compiles its own Cycles
  # translation units, which include these headers, and giving THEM /J
  # would hand unsigned char to every Qt, OCCT and FreeCAD header they
  # also include. Cycles cannot depend on it in any case -- it builds and
  # runs on x86-64 Linux, where plain char is signed -- so dropping it
  # leaves the whole process agreeing, which is the point of all this.
  if(CYCLES_EMBEDDED)
    add_compile_options(/nologo /Gd /bigobj /MP /utf-8)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
  else()
    set(CMAKE_CXX_FLAGS "/nologo /J /Gd /EHsc /bigobj /MP /std:c++17 /utf-8" CACHE STRING "MSVC MD C++ flags " FORCE)
    set(CMAKE_C_FLAGS "/nologo /J /Gd /MP /bigobj /utf-8" CACHE STRING "MSVC MD C++ flags " FORCE)

    if(CMAKE_CL_64)
      set(CMAKE_CXX_FLAGS_DEBUG "/Od /RTC1 /MDd /Zi" CACHE STRING "MSVC MD flags " FORCE)
    else()
      set(CMAKE_CXX_FLAGS_DEBUG "/Od /RTC1 /MDd /ZI" CACHE STRING "MSVC MD flags " FORCE)
    endif()
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /MD /DNDEBUG" CACHE STRING "MSVC MD flags " FORCE)
    set(CMAKE_CXX_FLAGS_MINSIZEREL "/O1 /Ob1 /MD /DNDEBUG" CACHE STRING "MSVC MD flags " FORCE)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Ob1 /MD /Zi /DNDEBUG" CACHE STRING "MSVC MD flags " FORCE)
    if(CMAKE_CL_64)
      set(CMAKE_C_FLAGS_DEBUG "/Od /RTC1 /MDd /Zi" CACHE STRING "MSVC MD flags " FORCE)
    else()
      set(CMAKE_C_FLAGS_DEBUG "/Od /RTC1 /MDd /ZI" CACHE STRING "MSVC MD flags " FORCE)
    endif()
    set(CMAKE_C_FLAGS_RELEASE "/O2 /Ob2 /MD /DNDEBUG" CACHE STRING "MSVC MD flags " FORCE)
    set(CMAKE_C_FLAGS_MINSIZEREL "/O1 /Ob1 /MD /DNDEBUG" CACHE STRING "MSVC MD flags " FORCE)
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "/O2 /Ob1 /MD /Zi /DNDEBUG" CACHE STRING "MSVC MD flags " FORCE)
  endif()

  list(APPEND PLATFORM_LINKLIBS psapi Version Dbghelp Shlwapi)

  # In MSVC 2019 the /Zc:inline option strips out the symbols generated by the
  # TF_REGISTRY_FUNCTION macro from USD, causing release builds to fail. So disable
  # the option there. See https://github.com/PixarAnimationStudios/USD/issues/1095.
  if(MSVC_VERSION GREATER_EQUAL 1920)
    string(APPEND CMAKE_CXX_FLAGS " /Zc:inline-")
  endif()

  if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /wd4996")
    string(APPEND CMAKE_CXX_FLAGS " /wd4244")
    string(APPEND CMAKE_CXX_FLAGS " /wd4267")
  endif()

  # Make Visual Studio Report __cplusplus version.
  string(APPEND CMAKE_CXX_FLAGS " /Zc:__cplusplus")

  add_definitions(-D_USE_MATH_DEFINES -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

# Enable SSE2NEON SIMD support if found.
#
if(SUPPORTS_NEON_BUILD AND SSE2NEON_FOUND)
  include_directories(SYSTEM "${SSE2NEON_INCLUDE_DIRS}")
  add_definitions(-DWITH_SSE2NEON)
endif()
