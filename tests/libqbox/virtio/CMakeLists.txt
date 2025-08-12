# virtio_mmio_blk_reset_test.cc removed - functionality merged into virtio_guest_driven_block_test.cc

add_executable(virtio_mmio_blk_test virtio_mmio_blk_test.cc)
install(TARGETS virtio_mmio_blk_test DESTINATION bin)
target_include_directories(virtio_mmio_blk_test PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/libqbox/include
    ${PROJECT_SOURCE_DIR}/qemu-components/common/include
    ${PROJECT_SOURCE_DIR}/qemu-components/reset_gpio/include
    ${PROJECT_SOURCE_DIR}/systemc-components/common/include)
target_link_libraries(virtio_mmio_blk_test PRIVATE
    virtio_mmio_blk
    router
    gs_memory
    reset_gpio
    ${TARGET_LIBS})

# Build VirtIO guest firmware using LLVM toolchain
set(FIRMWARE_ASM ${CMAKE_CURRENT_SOURCE_DIR}/virtio_guest_firmware.s)
set(FIRMWARE_OBJ ${CMAKE_CURRENT_BINARY_DIR}/virtio_guest_firmware.o)
set(FIRMWARE_ELF ${CMAKE_CURRENT_BINARY_DIR}/virtio_guest_firmware.elf)
set(FIRMWARE_BIN ${CMAKE_CURRENT_BINARY_DIR}/virtio_guest_firmware.bin)

# Define firmware address constants
set(MMIO_STATUS_BASE 0x80000000)
set(VIRTIO_DEVICE_BASE 0x10000000)
set(QUEUE_MEMORY_BASE 0x100010000)  # Within BULKMEM region (0x100000000 + 64KB offset)
set(DATA_BUFFER_BASE 0x100020000)   # Within BULKMEM region (0x100000000 + 128KB offset)

# Find LLVM tools
find_program(LLVM_MC llvm-mc REQUIRED)
find_program(LLD_LINKER ld.lld REQUIRED)
find_program(LLVM_OBJCOPY llvm-objcopy REQUIRED)

# Assemble the firmware
add_custom_command(
    OUTPUT ${FIRMWARE_OBJ}
    COMMAND ${LLVM_MC} -arch=aarch64 -filetype=obj -o ${FIRMWARE_OBJ} ${FIRMWARE_ASM}
    DEPENDS ${FIRMWARE_ASM}
    COMMENT "Assembling VirtIO guest firmware with llvm-mc"
)

# Link the firmware with address definitions
add_custom_command(
    OUTPUT ${FIRMWARE_ELF}
    COMMAND ${LLD_LINKER}
        -T ${CMAKE_CURRENT_SOURCE_DIR}/firmware.ld
        --defsym=MMIO_STATUS_BASE=${MMIO_STATUS_BASE}
        --defsym=VIRTIO_DEVICE_BASE=${VIRTIO_DEVICE_BASE}
        --defsym=QUEUE_MEMORY_BASE=${QUEUE_MEMORY_BASE}
        --defsym=DATA_BUFFER_BASE=${DATA_BUFFER_BASE}
        -o ${FIRMWARE_ELF} ${FIRMWARE_OBJ}
    DEPENDS ${FIRMWARE_OBJ} ${CMAKE_CURRENT_SOURCE_DIR}/firmware.ld
    COMMENT "Linking VirtIO guest firmware with ld.lld and address definitions"
)

# Extract binary firmware
add_custom_command(
    OUTPUT ${FIRMWARE_BIN}
    COMMAND ${LLVM_OBJCOPY} -O binary ${FIRMWARE_ELF} ${FIRMWARE_BIN}
    DEPENDS ${FIRMWARE_ELF}
    COMMENT "Extracting binary firmware with llvm-objcopy"
)

# Create a custom target for the firmware
add_custom_target(virtio_guest_firmware ALL DEPENDS ${FIRMWARE_BIN})

add_executable(virtio_guest_driven_block_test virtio_guest_driven_block_test.cc)
add_dependencies(virtio_guest_driven_block_test virtio_guest_firmware)
install(TARGETS virtio_guest_driven_block_test DESTINATION bin)
target_include_directories(virtio_guest_driven_block_test PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/libqbox/include
    ${PROJECT_SOURCE_DIR}/qemu-components/reset_gpio/include
    ${PROJECT_SOURCE_DIR}/systemc-components/common/include
    ${keystone_SOURCE_DIR}/include)
target_link_libraries(virtio_guest_driven_block_test PRIVATE
    cpu_arm_cortexA53
    virtio_mmio_blk
    global_peripheral_initiator
    router
    gs_memory
    reset_gpio
    ${TARGET_LIBS}
    keystone)

# Pass firmware path and address constants to the test
target_compile_definitions(virtio_guest_driven_block_test PRIVATE
    VIRTIO_FIRMWARE_PATH="${FIRMWARE_BIN}"
    FIRMWARE_MMIO_STATUS_BASE=${MMIO_STATUS_BASE}
    FIRMWARE_VIRTIO_DEVICE_BASE=${VIRTIO_DEVICE_BASE}
    FIRMWARE_QUEUE_MEMORY_BASE=${QUEUE_MEMORY_BASE}
    FIRMWARE_DATA_BUFFER_BASE=${DATA_BUFFER_BASE})

add_executable(virtio_mmio_net_test virtio_mmio_net_test.cc)
install(TARGETS virtio_mmio_net_test DESTINATION bin)
target_include_directories(virtio_mmio_net_test PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/libqbox/include
    ${PROJECT_SOURCE_DIR}/qemu-components/common/include
    ${PROJECT_SOURCE_DIR}/qemu-components/reset_gpio/include
    ${PROJECT_SOURCE_DIR}/systemc-components/common/include)
target_link_libraries(virtio_mmio_net_test PRIVATE
    virtio_mmio_net
    router
    gs_memory
    reset_gpio
    ${TARGET_LIBS})

add_test(NAME virtio_mmio_blk_test
    COMMAND $<TARGET_FILE:virtio_mmio_blk_test>)
add_test(NAME virtio_mmio_net_test
    COMMAND $<TARGET_FILE:virtio_mmio_net_test>)
add_test(NAME virtio_guest_driven_block_test
    COMMAND $<TARGET_FILE:virtio_guest_driven_block_test> --param test-bench.num_cpu=8)
