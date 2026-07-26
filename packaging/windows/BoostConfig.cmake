# Minimal BoostConfig for the Windows cross build. PlaneGCS uses Boost headers
# only, but CMake 3.30+ dropped the bundled FindBoost, so find_package(Boost)
# needs a package config that defines the Boost::boost interface target. We ship
# just the headers (Boost's own cross-build fails on its MPI component), so this
# config wires an INTERFACE target at the headers — no compiled Boost libraries.
set(Boost_VERSION 1.89.0)
set(Boost_FOUND TRUE)

get_filename_component(_boost_include_dir
  "${CMAKE_CURRENT_LIST_DIR}/../../../include" ABSOLUTE)

if(NOT TARGET Boost::headers)
  add_library(Boost::headers INTERFACE IMPORTED)
  set_target_properties(Boost::headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_boost_include_dir}")
endif()

# Boost::boost is the historical alias for the header-only target.
if(NOT TARGET Boost::boost)
  add_library(Boost::boost INTERFACE IMPORTED)
  set_target_properties(Boost::boost PROPERTIES
    INTERFACE_LINK_LIBRARIES Boost::headers)
endif()
