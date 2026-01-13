# Linker map file generation support for various linkers
# Supports: ld.lld (LLVM), ld.bfd (GNU), ld64 (Apple)

# Find Python3 for running the analysis script
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Function to enable linker map generation for a target
function(gs_generate_linker_map TARGET)
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
    add_custom_command(
        TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Linker map generated: ${MAP_FILE}"
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/get_libs.py ${MAP_FILE}
        VERBATIM
    )

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
