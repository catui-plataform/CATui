
add_library(pcsx2_libretro SHARED)

# PCSX2's interface target exports a C++ precompiled header.  This target also
# contains libretro-common C translation units, which MSVC cannot compile with
# that C++ PCH.
set_target_properties(pcsx2_libretro PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)

target_sources(pcsx2_libretro PRIVATE
	${CMAKE_SOURCE_DIR}/libretro/modern_main.cpp
	${CMAKE_SOURCE_DIR}/libretro/modern_host.cpp
)

if(WIN32)
   file(GLOB_RECURSE LRPS2_RESOURCE_FILES CONFIGURE_DEPENDS
      "${CMAKE_SOURCE_DIR}/bin/resources/*")
   set(LRPS2_RESOURCE_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/pcsx2-resources.zip")
   add_custom_command(
      OUTPUT "${LRPS2_RESOURCE_ARCHIVE}"
      COMMAND ${CMAKE_COMMAND} -E tar cf "${LRPS2_RESOURCE_ARCHIVE}" --format=zip resources
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
      DEPENDS ${LRPS2_RESOURCE_FILES}
      COMMENT "Packing PCSX2 runtime resources into the libretro core"
      VERBATIM
   )
   configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/modern_resources.rc.in"
      "${CMAKE_CURRENT_BINARY_DIR}/modern_resources.rc"
      @ONLY
   )
   set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/modern_resources.rc"
      PROPERTIES OBJECT_DEPENDS "${LRPS2_RESOURCE_ARCHIVE}")
   target_sources(pcsx2_libretro PRIVATE
      ${CMAKE_SOURCE_DIR}/libretro/modern_resources.cpp
      "${CMAKE_CURRENT_BINARY_DIR}/modern_resources.rc"
      "${LRPS2_RESOURCE_ARCHIVE}"
   )
   set_source_files_properties("${LRPS2_RESOURCE_ARCHIVE}" PROPERTIES HEADER_FILE_ONLY ON)
endif()

# getifaddrs()/freeifaddrs() are only declared by bionic from API 24 on,
# below what the NDK lanes target, so DEV9 gets libretro-common's netlink
# reimplementation instead. WANT_IFADDRS (set on PCSX2_FLAGS in
# pcsx2/CMakeLists.txt) is what switches the includes over. Every other
# platform has them in libc and must not compile this. Keep in step with
# Makefile.common.
if(ANDROID)
   target_sources(pcsx2_libretro PRIVATE
      ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/compat_ifaddrs.c
   )
endif()

# Which of rchd's hunk codecs are compiled in.  Stated here rather than
# left to rchd.c's fallback: that fallback turns on DEFLATE and LZMA
# unconditionally but gates FLAC and Zstandard behind HAVE_FLAC/HAVE_RFLAC
# and HAVE_ZSTD/HAVE_RZSTD, which this build has never defined.  The
# result was a reader that linked rflac.c and encoding_rzstd.c and then
# refused every 'cdfl' and 'cdzs' hunk with RCHD_ERROR_UNSUPPORTED - a
# per-hunk failure, so an image opened, reported correct geometry and a
# correct TOC, and only died on the fraction of hunks that happened to
# pick those codecs.  All four objects are in target_sources above, so
# all four codecs are named here.  Keep in step with Makefile.common.
target_compile_definitions(pcsx2_libretro PRIVATE
   NOMINMAX
   HAVE_RCHD_DEFLATE
   HAVE_RCHD_LZMA
   HAVE_RCHD_FLAC
   HAVE_RCHD_ZSTD
)

target_link_libraries(pcsx2_libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
)

if(WIN32)
  get_target_property(_pcsx2_interface_libs PCSX2_FLAGS INTERFACE_LINK_LIBRARIES)
  if(_pcsx2_interface_libs)
    list(REMOVE_ITEM _pcsx2_interface_libs dxcompiler.lib)
    set_target_properties(PCSX2_FLAGS PROPERTIES INTERFACE_LINK_LIBRARIES "${_pcsx2_interface_libs}")
  endif()
  # DEV9's winsock ethernet backend and adapter enumeration.
  target_link_libraries(pcsx2_libretro PRIVATE ws2_32 iphlpapi)
endif()

if(APPLE)
  target_link_libraries(pcsx2_libretro PRIVATE
    "-framework CoreGraphics"
    "-framework IOKit"
    "-framework Foundation"
  )
endif()

target_include_directories(PCSX2_FLAGS INTERFACE
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
)

target_include_directories(common PUBLIC
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
)

target_include_directories(pcsx2_libretro PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
	"${CMAKE_SOURCE_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/3rdparty/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

set_target_properties(pcsx2_libretro PROPERTIES
   LIBRARY_OUTPUT_NAME pcsx2_libretro
   PREFIX ""
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
   set(CLANG 1)
endif()

if(NOT MSVC AND NOT CLANG)
   set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
endif()

if(PACKAGE_MODE)
    install(TARGETS pcsx2_libretro DESTINATION ${BIN_DIR})
else(PACKAGE_MODE)
    install(TARGETS pcsx2_libretro DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif(PACKAGE_MODE)

# The smoke gate loads the core the moment it links and walks the
# lifecycle a frontend performs before any content exists: init, the
# BIOS-less retro_load_game failure path, deinit, unload.  A crash in
# any of it fails the build, so a broken artifact never reaches the
# buildbot.  Opt-in because it executes host code: CI turns it on for
# jobs whose runner natively runs what it just built; cross builds
# cannot.
option(LRPS2_SMOKE_GATE "Run the libretro lifecycle smoke gate after linking the core" OFF)
if(LRPS2_SMOKE_GATE)
   add_executable(lrps2_smoke ${CMAKE_SOURCE_DIR}/tests/smoke/lrps2_smoke.c)
   target_include_directories(lrps2_smoke PRIVATE
      "${CMAKE_SOURCE_DIR}/libretro/libretro-common/include")
   target_link_libraries(lrps2_smoke PRIVATE ${CMAKE_DL_LIBS})
   # The CI template builds only the pcsx2_libretro target, so the
   # harness rides along as its dependency.
   add_dependencies(pcsx2_libretro lrps2_smoke)
   # An empty working directory of its own: the BIOS scan must find
   # nothing, and anything the core writes stays contained.
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke-work")
   add_custom_command(TARGET pcsx2_libretro POST_BUILD
      COMMAND lrps2_smoke "$<TARGET_FILE:pcsx2_libretro>"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke-work"
      COMMENT "smoke gate: libretro lifecycle against the freshly linked core"
      VERBATIM)
endif()
