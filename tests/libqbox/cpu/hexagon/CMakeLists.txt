# Simple tests using Keystone
qbox_add_cpu_test(hexagon-ld-st-mmio-test 100 ld-st-mmio.cc)
qbox_add_cpu_test(hexagon-reset-test 100 hex-reset.cc)


# Find LLVM tools for external assembly
find_program(LLVM_MC llvm-mc)
find_program(LLD ld.lld)
find_program(LLVM_OBJCOPY llvm-objcopy)

if(LLVM_MC AND LLD AND LLVM_OBJCOPY)
    # Build assembly firmware for Hexagon DMI async invalidation test
    # Assemble the Hexagon DMI test assembly file
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.o
        COMMAND ${LLVM_MC} -arch=hexagon -filetype=obj
                ${CMAKE_CURRENT_SOURCE_DIR}/hexagon-dmi-test-async-inval.S
                -o ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.o
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hexagon-dmi-test-async-inval.S
        COMMENT "Assembling hexagon-dmi-test-async-inval.S"
    )

    # Link the DMI test object file
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.elf
        COMMAND ${LLD} -T ${CMAKE_CURRENT_SOURCE_DIR}/hexagon-dmi-test-async-inval.ld
                ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.o
                -o ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.elf
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.o
                ${CMAKE_CURRENT_SOURCE_DIR}/hexagon-dmi-test-async-inval.ld
        COMMENT "Linking hexagon-dmi-test-async-inval.elf"
    )

    # Create DMI test binary file
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.bin
        COMMAND ${LLVM_OBJCOPY} -O binary
                ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.elf
                ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.bin
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.elf
        COMMENT "Creating hexagon-dmi-test-async-inval.bin"
    )

    # Create a custom target for the DMI test firmware
    add_custom_target(hexagon-dmi-test-async-inval-firmware
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.bin)

    qbox_add_cpu_test(hexagon-dmi-test-async-inval 100 dmi-test-async-inval.cc)
    add_dependencies(hexagon-dmi-test-async-inval hexagon-dmi-test-async-inval-firmware)
    target_compile_definitions(hexagon-dmi-test-async-inval PRIVATE
        FIRMWARE_BIN_PATH="${CMAKE_CURRENT_BINARY_DIR}/hexagon-dmi-test-async-inval.bin")
else()
    message(WARNING "LLVM tools not found - skipping hexagon-dmi-test-async-inval")
endif()

