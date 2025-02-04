set(ZLIB_LIBRARIES zlib)
set(CRASHPAD_INSTALL_HANDLER ON)
set(CRASHPAD_DIR ${ANDROID_QEMU2_TOP_DIR}/../crashpad/)

# Make sure we do not have definitions that will break us.
remove_definitions(
  "-DNDEBUG"
  "-DNDEBUG=1"
  "-D_FILE_OFFSET_BITS=64"
  "-D_GNU_SOURCE=1"
  "-D_LARGEFILE_SOURCE"
  "-D_LIBCPP_VERSION=__GLIBCPP__"
  "-D__STDC_CONSTANT_MACROS"
  "-D__STDC_FORMAT_MACROS=1"
  "-D__STDC_LIMIT_MACROS=1"
  "-D_DARWIN_C_SOURCE=1"
  "-DANDROID_DEBUG")

# ==============================================================================
# Compiles MASM assembly files into object files (Windows-specific).
#
# Args: TARGET: The name of the target to which the compiled object files will
# be linked (Required). SRC: A list of MASM assembly source files (.asm) to
# compile.
# ==============================================================================
function(masm_compile)
  set(options)
  set(oneValueArgs TARGET)
  set(multiValueArgs SRC)
  cmake_parse_arguments(MASM "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})
  target_sources(${MASM_TARGET} PRIVATE ${OBJ_OUTPUT_PATH})
endfunction()

# These tests are flaky on mac, and fail often.
set(CRASHPAD_UTIL_TEST_PARAMS
    "--gtest_filter=-ServiceManagement.SubmitRemoveJob")

add_subdirectory(${CRASHPAD_DIR} crashpad)

if(WINDOWS_MSVC_X86_64)
  # Remove unneeded compat layer that will introduce unwanted compiler flags.
  get_target_property(ALL_WIN_LINK_LIBS crashpad_util_win LINK_LIBRARIES)
  list(REMOVE_ITEM ALL_WIN_LINK_LIBS msvc-posix-compat)
  set_target_properties(crashpad_util_win PROPERTIES LINK_LIBRARIES
                                                     "${ALL_WIN_LINK_LIBS}")
endif()

android_license(TARGET "crashpad_internal" LIBNAME None SPDX None LICENSE None
                LOCAL None)
android_license(TARGET "bsm" LIBNAME None SPDX None LICENSE None LOCAL None)
