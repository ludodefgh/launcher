cmake_minimum_required(VERSION 3.16)

# ESP-IDF's own git-describe-based PROJECT_VER auto-detection
# (tools/cmake/project.cmake -> git_describe()) silently fails in this
# project's docker-based build (always falls back to a hardcoded "1",
# logged as "Could not use 'git describe' to determine PROJECT_VER" even
# with a real annotated tag present and plain `git describe` working fine
# from the same shell) -- a known class of flakiness across different
# git/cmake/container environments, not specific to a bug here. Setting
# PROJECT_VER explicitly beforehand skips that detection entirely (it only
# runs `if(NOT DEFINED PROJECT_VER)`), which is the standard workaround.
execute_process(
    # -c safe.directory=*: this build is routinely run in a fresh Docker
    # container (see CLAUDE.md "Useful commands") with the repo bind-mounted
    # from the host -- git treats a repo owned by a different UID than the
    # current user as "dubious ownership" and refuses to touch it unless
    # told otherwise. Scoped inline (not a global git config change) so it
    # doesn't require any one-time setup and doesn't affect anything outside
    # this single command.
    COMMAND git -c safe.directory=* describe --always --tags --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PROJECT_VER)
    set(PROJECT_VER "unknown")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(launcher)
