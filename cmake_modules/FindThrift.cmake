# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

#########
#
# Locate and configure the Thrift library.
# Defines the following variables:
#
#   THRIFT_INCLUDE_DIR - the include directory for thrift headers
#   THRIFT_SHARED_LIBRARY - path to thrift's shared library
#   THRIFT_STATIC_LIBRARY - path to thrift's static library
#   THRIFT_EXECUTABLE - the thrift compiler
#   THRIFT_FOUND - whether the Thrift library and executable has been found
#
#  ====================================================================
#  Example:
#
#   find_package(Thrift REQUIRED)
#   include_directories(${THRIFT_INCLUDE_DIR})
#
#   include_directories(${CMAKE_CURRENT_BINARY_DIR})
#   THRIFT_GENERATE_CPP(THRIFT_SRCS THRIFT_HDRS THRIFT_TGTS
#     [SOURCE_ROOT <root from which source is found>]
#     [BINARY_ROOT <root into which binaries are built>]
#     THRIFT_FILES foo.thrift)
#   add_executable(bar bar.cc ${THRIFT_SRCS} ${THRIFT_HDRS})
#   target_link_libraries(bar ${THRIFT_SHARED_LIBRARY})
#
#  ====================================================================
#
# THRIFT_GENERATE_CPP (public function)
#   SRCS = Variable to define with autogenerated
#          source files
#   HDRS = Variable to define with autogenerated
#          header files
#   TGTS = Variable to define with autogenerated
#          custom targets; if SRCS/HDRS need to be used in multiple
#          libraries, those libraries should depend on these targets
#          in order to "serialize" the thrift invocations
#   FB303 = Option which determines if the Thrift definitions depend on the
#           FB303 support library.
#  ====================================================================

function(THRIFT_GENERATE_CPP SRCS HDRS TGTS)
  if(NOT ARGN)
    message(SEND_ERROR "Error: THRIFT_GENERATE_CPP() called without any thrift files")
    return()
  endif(NOT ARGN)

  set(options FB303)
  set(one_value_args SOURCE_ROOT BINARY_ROOT)
  set(multi_value_args EXTRA_THRIFT_PATHS THRIFT_FILES EXTRA_OPTIONS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(SEND_ERROR "Error: unrecognized arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()

  set(${SRCS})
  set(${HDRS})
  set(${TGTS})

  set(EXTRA_THRIFT_PATH_ARGS)
  foreach(PP ${ARG_EXTRA_THRIFT_PATHS})
    set(EXTRA_THRIFT_PATH_ARGS ${EXTRA_THRIFT_PATH_ARGS} -I ${PP})
  endforeach()

  if("${ARG_SOURCE_ROOT}" STREQUAL "")
    SET(ARG_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  GET_FILENAME_COMPONENT(ARG_SOURCE_ROOT ${ARG_SOURCE_ROOT} ABSOLUTE)

  if("${ARG_BINARY_ROOT}" STREQUAL "")
    SET(ARG_BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  GET_FILENAME_COMPONENT(ARG_BINARY_ROOT ${ARG_BINARY_ROOT} ABSOLUTE)

  foreach(FIL ${ARG_THRIFT_FILES})
    get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
    get_filename_component(FIL_WE ${FIL} NAME_WE)

    set(THRIFT_H_OUT "${ARG_BINARY_ROOT}/${FIL_WE}_types.h" "${ARG_BINARY_ROOT}/${FIL_WE}_constants.h")
    set(THRIFT_CC_OUT "${ARG_BINARY_ROOT}/${FIL_WE}_constants.cpp" "${ARG_BINARY_ROOT}/${FIL_WE}_types.cpp")

    execute_process(COMMAND awk "/^service/ { print $2 }" "${ABS_FIL}"
                    OUTPUT_VARIABLE SERVICES
                    OUTPUT_STRIP_TRAILING_WHITESPACE)

    foreach(SERVICE ${SERVICES})
      list(APPEND THRIFT_H_OUT "${ARG_BINARY_ROOT}/${SERVICE}.h")
      list(APPEND THRIFT_CC_OUT "${ARG_BINARY_ROOT}/${SERVICE}.cpp")
    endforeach()

    list(APPEND ${SRCS} "${THRIFT_CC_OUT}")
    list(APPEND ${HDRS} "${THRIFT_H_OUT}")

    if(ARG_FB303)
      list(APPEND ${SRCS} fb303_types.cpp FacebookService.cpp)
      list(APPEND ${HDRS} fb303_types.h FacebookService.h)
    endif()

    # The `EXTRA_OPTIONS` argument allows passing additional flags to the Thrift compiler
    # for specific invocations. This is useful to suppress warnings only for selected files.
    #
    # For example, the `-nowarn` flag is used with `hive_metastore.thrift`, which is copied
    # from the Hive project and cannot be modified. This avoids noisy warnings about missing
    # field IDs without globally muting all warnings in unrelated Thrift files.
    add_custom_command(
      OUTPUT ${THRIFT_CC_OUT} ${THRIFT_H_OUT}
      DEPENDS ${ABS_FIL}
      COMMAND  ${THRIFT_EXECUTABLE}
      ARGS
        --gen cpp:moveable_types
        --recurse
        --out ${ARG_BINARY_ROOT}
        -I ${ARG_SOURCE_ROOT}
        # Used to find built-in .thrift files (e.g. fb303.thrift)
        -I ${THIRDPARTY_INSTALL_CURRENT_DIR}
        ${ARG_EXTRA_OPTIONS}
        ${EXTRA_THRIFT_PATH_ARGS} ${ABS_FIL}
      COMMENT "Running C++ thrift compiler on ${FIL}"
      VERBATIM )

    # This custom target enforces that there's just one invocation of thrift
    # when there are multiple consumers of the generated files. The target name
    # must be unique; adding parts of the filename helps ensure this.
    string(MAKE_C_IDENTIFIER "${ARG_BINARY_ROOT}${FIL}" TGT_NAME)
    add_custom_target(${TGT_NAME}
      DEPENDS "${THRIFT_CC_OUT}" "${THRIFT_H_OUT}")
    list(APPEND ${TGTS} "${TGT_NAME}")
  endforeach()

  set_source_files_properties(${${SRCS}} ${${HDRS}} PROPERTIES GENERATED TRUE)
  set(${SRCS} ${${SRCS}} PARENT_SCOPE)
  set(${HDRS} ${${HDRS}} PARENT_SCOPE)
  set(${TGTS} ${${TGTS}} PARENT_SCOPE)
endfunction()

find_path(THRIFT_INCLUDE_DIR thrift/Thrift.h
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)

find_library(THRIFT_SHARED_LIBRARY thrift
             DOC "The Thrift Library"
             NO_CMAKE_SYSTEM_PATH
             NO_SYSTEM_ENVIRONMENT_PATH)

find_library(THRIFT_STATIC_LIBRARY libthrift.a
  DOC "Static version of the Thrift Library"
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)

find_program(THRIFT_EXECUTABLE thrift
  DOC "The Thrift Compiler"
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Thrift REQUIRED_VARS
  THRIFT_SHARED_LIBRARY THRIFT_STATIC_LIBRARY
  THRIFT_INCLUDE_DIR THRIFT_EXECUTABLE)
