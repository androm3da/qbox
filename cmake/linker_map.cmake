# Linker map file generation support for various linkers
# Supports: ld.lld (LLVM), ld.bfd (GNU), ld64 (Apple)

# Find Python3 for running the analysis script
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Store the path to the scripts directory relative to this cmake file
# This works correctly even when QBox is used as a CPM dependency
set(QBOX_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../scripts")

# Function to enable linker map generation for a target
function(gs_generate_linker_map TARGET)
    # Check if target exists
    if(NOT TARGET ${TARGET})
        message(FATAL_ERROR "gs_generate_linker_map called with non-existent target '${TARGET}'\n"
                            "Make sure to call gs_generate_linker_map() AFTER the target is created with add_executable() or add_library()")
    endif()

    # Determine the linker being used
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE)
        # Clang on non-Apple platforms typically uses lld or can use bfd
        if(GS_ENABLE_LLD)
            # Using lld explicitly
            set(LINKER_TYPE "lld")
        else()
            # Default to bfd-style flags (works with both bfd and gold)
            set(LINKER_TYPE "bfd")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # GCC typically uses ld.bfd
        set(LINKER_TYPE "bfd")
    elseif(APPLE)
        # macOS uses ld64
        set(LINKER_TYPE "ld64")
    else()
        # Default to bfd-style for unknown compilers
        set(LINKER_TYPE "bfd")
    endif()

    # Get the target type
    get_target_property(TARGET_TYPE ${TARGET} TYPE)

    # Check if this is an INTERFACE library
    if(TARGET_TYPE STREQUAL "INTERFACE_LIBRARY")
        message(STATUS "Skipping linker map generation for INTERFACE library '${TARGET}'")
        return()
    endif()

    # Only generate maps for executables and shared/static libraries
    if(NOT (TARGET_TYPE STREQUAL "EXECUTABLE" OR
            TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR
            TARGET_TYPE STREQUAL "STATIC_LIBRARY" OR
            TARGET_TYPE STREQUAL "MODULE_LIBRARY"))
        message(STATUS "Skipping linker map generation for target '${TARGET}' of type '${TARGET_TYPE}'")
        return()
    endif()

    # Set the map file path
    set(MAP_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.map")

    # Apply linker flags based on linker type
    if(LINKER_TYPE STREQUAL "lld")
        # LLVM lld
        target_link_options(${TARGET} PRIVATE
            "-Wl,-Map=${MAP_FILE}"
        )
    elseif(LINKER_TYPE STREQUAL "bfd")
        # GNU ld (ld.bfd) and gold
        target_link_options(${TARGET} PRIVATE
            "-Wl,-Map=${MAP_FILE}"
        )
    elseif(LINKER_TYPE STREQUAL "ld64")
        # Apple ld64
        target_link_options(${TARGET} PRIVATE
            "-Wl,-map,${MAP_FILE}"
        )
    endif()

    # Create a custom target to ensure the map file is generated and analyzed
    # Check if the get_libs.py script exists (it might not when QBox is a dependency)
    if(EXISTS "${QBOX_SCRIPTS_DIR}/get_libs.py")
        add_custom_command(
            TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Linker map generated: ${MAP_FILE}"
            COMMAND ${Python3_EXECUTABLE} ${QBOX_SCRIPTS_DIR}/get_libs.py ${MAP_FILE}
            VERBATIM
        )
    else()
        add_custom_command(
            TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Linker map generated: ${MAP_FILE}"
            COMMAND ${CMAKE_COMMAND} -E echo "Note: Dependency analysis script not found at ${QBOX_SCRIPTS_DIR}/get_libs.py"
            COMMAND ${CMAKE_COMMAND} -E echo "      Run manually: python3 <qbox>/scripts/get_libs.py ${MAP_FILE}"
            VERBATIM
        )
    endif()

    # Set a property to track that this target has a map file
    set_target_properties(${TARGET} PROPERTIES
        LINKER_MAP_FILE "${MAP_FILE}"
    )
endfunction()

# Function to enable linker maps for all targets in a directory
function(gs_enable_linker_maps_for_all_targets)
    # This function should be called after all targets are defined
    # It will iterate through all targets and enable map generation

    # Get all targets
    get_property(ALL_TARGETS DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)

    foreach(TARGET ${ALL_TARGETS})
        get_target_property(TARGET_TYPE ${TARGET} TYPE)
        # Only generate maps for executables and shared libraries
        if(TARGET_TYPE STREQUAL "EXECUTABLE" OR TARGET_TYPE STREQUAL "SHARED_LIBRARY")
            gs_generate_linker_map(${TARGET})
        endif()
    endforeach()
endfunction()

# Option to enable linker map generation globally
option(GS_GENERATE_LINKER_MAPS "Generate linker map files for all executables and libraries" OFF)

# If enabled globally, set up a deferred call to enable maps for all targets
if(GS_GENERATE_LINKER_MAPS)
    cmake_language(DEFER CALL gs_enable_linker_maps_for_all_targets)
endif()
