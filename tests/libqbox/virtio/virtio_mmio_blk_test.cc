/*
 * This file is part of libqbox
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <systemc>
#include <memory>
#include <unistd.h>

#include <cci_configuration>
#include <cci/utils/broker.h>
#include <libgsutils.h>

#include <gs_memory.h>
#include <router.h>
#include <virtio_mmio_blk.h>
#include <qemu-instance.h>
#include <ports/multiinitiator-signal-socket.h>
#include <reset_gpio.h>

#include "test/test.h"
#include <tests/initiator-tester.h>
#include <scp/report.h>
#include "virtio_test_common.h"
#include "virtio_blk_test_helpers.h"

/*
 * VirtIO MMIO Block Device I/O Test with Device Reset Iterations
 *
 * This test performs multiple iterations of VirtIO block I/O operations with device resets:
 * 1. Creates a 64MiB virtual block device backed by a temporary file
 * 2. Uses InitiatorTester to perform VirtIO block I/O operations
 * 3. Tests in-range block reads/writes expecting VIRTIO_BLK_S_OK
 * 4. Tests out-of-range access expecting VIRTIO_BLK_S_IOERR
 * 5. Verifies data integrity for successful operations
 * 6. Performs device reset and repeats the tests multiple times
 */
class VirtioMmioBlkTest : public TestBench
{
public:
    // Block-specific constants
    static constexpr uint64_t BLOCK_SIZE = 512;               // Standard sector size
    static constexpr uint64_t DEVICE_SIZE = 64 * 1024 * 1024; // 64MiB
    static constexpr uint64_t NUM_SECTORS = DEVICE_SIZE / BLOCK_SIZE;

private:
    QemuInstanceManager m_inst_manager;
    QemuInstance m_qemu_inst;
    std::unique_ptr<virtio_mmio_blk> m_blk_device;
    gs::router<> m_router;
    InitiatorTester m_initiator;
    VirtIOBlockTestHelper<InitiatorTester> m_virtio_helper;
    std::string m_temp_filename;

    // Reset infrastructure
    reset_gpio m_reset_controller;

    bool m_test_completed;
    bool m_test_passed;
    int m_current_iteration;
    int m_successful_iterations;

public:
    SC_HAS_PROCESS(VirtioMmioBlkTest);

    VirtioMmioBlkTest(const sc_core::sc_module_name& n)
        : TestBench(n)
        , m_qemu_inst("qemu_inst", &m_inst_manager, qemu::Target::AARCH64)
        , m_router("router")
        , m_initiator("initiator")
        , m_virtio_helper(m_initiator, this)
        , m_reset_controller("reset_controller", m_qemu_inst)
        , m_test_completed(false)
        , m_test_passed(false)
        , m_current_iteration(0)
        , m_successful_iterations(0)
    {
        // Create temporary file for block device
        char temp_filename[] = "/tmp/qbox_virtio_simple_test_XXXXXX";
        int fd = mkstemp(temp_filename);
        if (fd == -1) {
            SCP_FATAL(SCMOD) << "Failed to create temporary file for block device";
        }

        // Create minimal device file
        if (ftruncate(fd, DEVICE_SIZE) != 0) {
            SCP_FATAL(SCMOD) << "Failed to set block device size";
        }
        close(fd);

        m_temp_filename = temp_filename;

        // Configure CCI parameters for the VirtIO device
        std::string full_name = std::string(name()) + ".virtio_blk";
        std::string blkdev_config = "file=" + m_temp_filename + ",format=raw,if=none";

        cci::cci_get_broker().set_preset_cci_value(full_name + ".blkdev_str", cci::cci_value(blkdev_config));
        cci::cci_get_broker().set_preset_cci_value(full_name + ".mem.address",
                                                   cci::cci_value(VirtIOTestCommon::VIRTIO_BASE));
        cci::cci_get_broker().set_preset_cci_value(full_name + ".mem.size",
                                                   cci::cci_value(VirtIOTestCommon::VIRTIO_SIZE));
        cci::cci_get_broker().set_preset_cci_value(full_name + ".mem.relative_addresses", cci::cci_value(true));

        SCP_INFO(SCMOD) << "Creating simple VirtIO block device test with " << (DEVICE_SIZE / (1024 * 1024))
                        << "MiB backing file";

        // Create the VirtIO block device
        m_blk_device = std::make_unique<virtio_mmio_blk>("virtio_blk", m_qemu_inst);

        // Connect device to router
        m_router.add_target(m_blk_device->socket, VirtIOTestCommon::VIRTIO_BASE, VirtIOTestCommon::VIRTIO_SIZE);
        m_initiator.socket.bind(m_router.target_socket);

        // Note: reset controller will be triggered directly via reset_in.write()

        // Start iterative block I/O test with resets
        SC_THREAD(run_iterative_tests);
    }

    virtual ~VirtioMmioBlkTest()
    {
        if (!m_temp_filename.empty()) {
            unlink(m_temp_filename.c_str());
        }
    }

    void run_iterative_tests()
    {
        SCP_INFO(SCMOD) << "Starting iterative VirtIO block I/O tests with device resets ("
                        << VirtIOTestCommon::NUM_ITERATIONS << " iterations)";

        for (m_current_iteration = 1; m_current_iteration <= VirtIOTestCommon::NUM_ITERATIONS; m_current_iteration++) {
            SCP_INFO(SCMOD) << "=== Iteration " << m_current_iteration << "/" << VirtIOTestCommon::NUM_ITERATIONS
                            << " ===";

            // Wait for device initialization (longer wait on first iteration)
            if (m_current_iteration == 1) {
                sc_core::wait(100, sc_core::SC_MS);
            } else {
                sc_core::wait(50, sc_core::SC_MS); // Shorter wait after reset
            }

            // Perform the I/O test suite
            bool iteration_passed = perform_single_io_test();

            if (iteration_passed) {
                m_successful_iterations++;
                SCP_INFO(SCMOD) << "Iteration " << m_current_iteration << " PASSED";
            } else {
                SCP_ERR(SCMOD) << "Iteration " << m_current_iteration << " FAILED";
                // Continue with remaining iterations even if one fails
            }

            // Perform device reset before next iteration (except on last iteration)
            if (m_current_iteration < VirtIOTestCommon::NUM_ITERATIONS) {
                SCP_INFO(SCMOD) << "Performing device reset before iteration " << (m_current_iteration + 1);
                perform_device_reset();
                sc_core::wait(100, sc_core::SC_MS); // Wait for reset to complete
            }
        }

        // Determine overall test result
        SCP_INFO(SCMOD) << "=== Test Summary ===";
        SCP_INFO(SCMOD) << "Successful iterations: " << m_successful_iterations << "/"
                        << VirtIOTestCommon::NUM_ITERATIONS;

        if (m_successful_iterations == VirtIOTestCommon::NUM_ITERATIONS) {
            SCP_INFO(SCMOD) << "All iterations passed successfully!";
            m_test_passed = true;
        } else {
            SCP_ERR(SCMOD) << "Some iterations failed - test incomplete";
            m_test_passed = false;
        }

        m_test_completed = true;
        sc_core::sc_stop();
    }

    bool perform_single_io_test()
    {
        SCP_INFO(SCMOD) << "Starting VirtIO block I/O test suite for iteration " << m_current_iteration;

        try {
            // Test 1: In-range block read with expected VIRTIO_BLK_S_OK
            SCP_INFO(SCMOD) << "Test 1: In-range block read expecting VIRTIO_BLK_S_OK";
            std::vector<uint8_t> read_data;
            uint8_t status;
            uint64_t test_sector = 100 + (m_current_iteration * 10); // Vary sector per iteration

            if (!m_virtio_helper.read_block(VirtIOTestCommon::VIRTIO_BASE, test_sector, read_data, status)) {
                SCP_ERR(SCMOD) << "Block read operation failed";
                return false;
            }

            if (status != VIRTIO_BLK_S_OK) {
                SCP_ERR(SCMOD) << "Test 1 FAILED: Expected VIRTIO_BLK_S_OK (0), got status " << (int)status;
                return false;
            }

            // Verify read data matches expected pattern
            if (!VirtIOBlockTestHelper<InitiatorTester>::verify_test_pattern(read_data, test_sector)) {
                SCP_ERR(SCMOD) << "Test 1 FAILED: Read data doesn't match expected pattern for sector " << test_sector;
                return false;
            }

            SCP_INFO(SCMOD) << "Test 1 PASSED: In-range read returned VIRTIO_BLK_S_OK with correct data";

            // Test 2: In-range block write with expected VIRTIO_BLK_S_OK
            SCP_INFO(SCMOD) << "Test 2: In-range block write expecting VIRTIO_BLK_S_OK";
            uint64_t write_sector = 200 + (m_current_iteration * 20); // Vary sector per iteration
            auto write_data = VirtIOBlockTestHelper<InitiatorTester>::generate_test_pattern(write_sector);

            if (!m_virtio_helper.write_block(VirtIOTestCommon::VIRTIO_BASE, write_sector, write_data, status)) {
                SCP_ERR(SCMOD) << "Block write operation failed";
                return false;
            }

            if (status != VIRTIO_BLK_S_OK) {
                SCP_ERR(SCMOD) << "Test 2 FAILED: Expected VIRTIO_BLK_S_OK (0), got status " << (int)status;
                return false;
            }

            SCP_INFO(SCMOD) << "Test 2 PASSED: In-range write returned VIRTIO_BLK_S_OK";

            // Test 3: Read back written data to verify integrity
            SCP_INFO(SCMOD) << "Test 3: Read back written data to verify integrity";
            std::vector<uint8_t> verify_data;

            if (!m_virtio_helper.read_block(VirtIOTestCommon::VIRTIO_BASE, write_sector, verify_data, status)) {
                SCP_ERR(SCMOD) << "Block read-back operation failed";
                return false;
            }

            if (status != VIRTIO_BLK_S_OK) {
                SCP_ERR(SCMOD) << "Test 3 FAILED: Read-back expected VIRTIO_BLK_S_OK (0), got status " << (int)status;
                return false;
            }

            if (!VirtIOBlockTestHelper<InitiatorTester>::verify_test_pattern(verify_data, write_sector)) {
                SCP_ERR(SCMOD) << "Test 3 FAILED: Read-back data doesn't match written data";
                return false;
            }

            SCP_INFO(SCMOD) << "Test 3 PASSED: Read-back data matches written data with VIRTIO_BLK_S_OK";

            // Test 4: Out-of-range read expecting VIRTIO_BLK_S_IOERR
            SCP_INFO(SCMOD) << "Test 4: Out-of-range block read expecting VIRTIO_BLK_S_IOERR";
            uint64_t invalid_sector = NUM_SECTORS + 1000; // Well beyond device capacity
            std::vector<uint8_t> oob_read_data;

            if (!m_virtio_helper.read_block(VirtIOTestCommon::VIRTIO_BASE, invalid_sector, oob_read_data, status)) {
                SCP_ERR(SCMOD) << "Out-of-range read operation processing failed";
                return false;
            }

            if (status != VIRTIO_BLK_S_IOERR) {
                SCP_ERR(SCMOD) << "Test 4 FAILED: Expected VIRTIO_BLK_S_IOERR (1), got status " << (int)status;
                return false;
            }

            SCP_INFO(SCMOD) << "Test 4 PASSED: Out-of-range read correctly returned VIRTIO_BLK_S_IOERR";

            // Test 5: Out-of-range write expecting VIRTIO_BLK_S_IOERR
            SCP_INFO(SCMOD) << "Test 5: Out-of-range block write expecting VIRTIO_BLK_S_IOERR";
            auto oob_write_data = VirtIOBlockTestHelper<InitiatorTester>::generate_test_pattern(invalid_sector);

            if (!m_virtio_helper.write_block(VirtIOTestCommon::VIRTIO_BASE, invalid_sector, oob_write_data, status)) {
                SCP_ERR(SCMOD) << "Out-of-range write operation processing failed";
                return false;
            }

            if (status != VIRTIO_BLK_S_IOERR) {
                SCP_ERR(SCMOD) << "Test 5 FAILED: Expected VIRTIO_BLK_S_IOERR (1), got status " << (int)status;
                return false;
            }

            SCP_INFO(SCMOD) << "Test 5 PASSED: Out-of-range write correctly returned VIRTIO_BLK_S_IOERR";

            SCP_INFO(SCMOD) << "All VirtIO block I/O tests passed for iteration " << m_current_iteration;
            return true;

        } catch (const std::exception& e) {
            SCP_ERR(SCMOD) << "Exception during VirtIO block I/O testing in iteration " << m_current_iteration << ": "
                           << e.what();
            return false;
        }
    }

    void perform_device_reset()
    {
        SCP_INFO(SCMOD) << "Initiating device reset via QEMU instance";

        try {
            // Call reset directly on the QEMU instance
            // This is simpler and avoids the MultiInitiatorSignalSocket complexity
            m_qemu_inst.get().system_reset();

            // Wait for reset to complete
            sc_core::wait(200, sc_core::SC_MS);

            SCP_INFO(SCMOD) << "Device reset completed successfully";

        } catch (const std::exception& e) {
            SCP_ERR(SCMOD) << "Exception during device reset: " << e.what();
        }
    }

    virtual void end_of_simulation() override
    {
        TestBench::end_of_simulation();

        TEST_ASSERT(m_test_completed);
        TEST_ASSERT(m_test_passed);
        TEST_ASSERT(m_successful_iterations == VirtIOTestCommon::NUM_ITERATIONS);

        SCP_INFO(SCMOD) << "Iterative VirtIO MMIO test with device resets completed successfully";
        SCP_INFO(SCMOD) << "Successfully completed " << m_successful_iterations << "/"
                        << VirtIOTestCommon::NUM_ITERATIONS << " iterations";
    }
};

int sc_main(int argc, char* argv[]) { return run_testbench<VirtioMmioBlkTest>(argc, argv); }
