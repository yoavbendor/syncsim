# SPDX-License-Identifier: Apache-2.0
#
# syncsim ns-3 track -- Tier 3 / P3b: the generic YAML-driven scenario engine.
#
# This scratch subdirectory carries its own CMakeLists.txt so it can link an
# EXTERNAL library (yaml-cpp) without patching ns-3 core -- exactly the mechanism
# ns-3.45's own scratch/CMakeLists.txt documents ("If the subdirectory contains a
# CMakeLists.txt file we let the CMake file manage the source files... Use this if
# you want to link to external libraries without creating a module"). It calls the
# same build_exec() helper the auto-scan uses (see build-support/custom-modules/
# ns3-executables.cmake), just with yaml-cpp added to LIBRARIES_TO_LINK alongside
# the standard ${ns3-libs}/${ns3-contrib-libs}.
#
# yaml-cpp (MIT-licensed, mature, standard) is found via CMake's config package if
# present, else via a plain library search -- both cover the Debian/Ubuntu
# libyaml-cpp-dev packaging used in this project's Docker ns3 stage and sandbox.

find_package(yaml-cpp QUIET)

# Resolve the yaml-cpp target/library across the packaging variants seen in the
# wild: a modern config package exports the `yaml-cpp` target; older/plain
# installs expose only the library + headers, which we locate by hand.
set(SYNCSIM_YAMLCPP "")
if(TARGET yaml-cpp::yaml-cpp)
  set(SYNCSIM_YAMLCPP yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
  set(SYNCSIM_YAMLCPP yaml-cpp)
else()
  find_library(YAMLCPP_LIB NAMES yaml-cpp)
  if(YAMLCPP_LIB)
    set(SYNCSIM_YAMLCPP ${YAMLCPP_LIB})
  else()
    message(FATAL_ERROR
      "syncsim/sim: yaml-cpp not found. Install libyaml-cpp-dev "
      "(apt-get install -y --no-install-recommends libyaml-cpp-dev).")
  endif()
endif()

build_exec(
  EXECNAME sim
  EXECNAME_PREFIX scratch_syncsim-sim_
  SOURCE_FILES sim.cc gptp.cc clock.cc
  LIBRARIES_TO_LINK ${SYNCSIM_YAMLCPP} "${ns3-libs}" "${ns3-contrib-libs}"
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/syncsim-sim
)
