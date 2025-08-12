/*
 * This file is part of libqbox
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <systemc>
#include <memory>
#include <tlm>

#include <cci_configuration>
#include <cci/utils/broker.h>
#include <libgsutils.h>

#include <gs_memory.h>
#include <router.h>
#include <virtio_mmio_net.h>
#include <qemu-instance.h>
#include <ports/multiinitiator-signal-socket.h>
#include <reset_gpio.h>

#include "test/test.h"
#include <tests/initiator-tester.h>
#include <scp/report.h>
#include "virtio_test_common.h"

using namespace VirtIOTestCommon;

/*
 * VirtIO MMIO Network Device Basic Test with Device Reset Iterations
 *
 * This test performs multiple iterations of VirtIO network device initialization and basic setup with device resets:
 * 1. Creates a VirtIO network device with basic user-mode networking
 * 2. Tests device initialization and feature negotiation
 * 3. Verifies device magic value, version, and device ID
 * 4. Tests basic device configuration access
 * 5. Performs device reset and repeats the tests multiple times
 */
class VirtioMmioNetTest : public TestBench
{
public:
private:
    QemuInstanceManager m_inst_manager;
    QemuInstance m_qemu_inst;
    std::unique_ptr<virtio_mmio_net> m_net_device;
    gs::router<> m_router;
    InitiatorTester m_initiator;

    // Reset infrastructure
    reset_gpio m_reset_controller;

    bool m_test_completed;
    bool m_test_passed;
    int m_current_iteration;
    int m_successful_iterations;

public:
    SC_HAS_PROCESS(VirtioMmioNetTest);

    VirtioMmioNetTest(const sc_core::sc_module_name& n)
        : TestBench(n)
        , m_qemu_inst("qemu_inst", &m_inst_manager, qemu::Target::AARCH64)
        , m_router("router")
        , m_initiator("initiator")
        , m_reset_controller("reset_controller", m_qemu_inst)
        , m_test_completed(false)
        , m_test_passed(false)
        , m_current_iteration(0)
        , m_successful_iterations(0)
    {
        // Configure CCI parameters for the VirtIO network device
        std::string full_name = std::string(name()) + ".virtio_net";

        // Use simple user-mode networking (no external network dependencies)
        std::string netdev_config = "user,restrict=on";

        cci::cci_get_broker().set_preset_cci_value(full_name + ".netdev_str", cci::cci_value(netdev_config));
        cci::cci_get_broker().set_preset_cci_value(full_name + ".mem.address", cci::cci_value(VIRTIO_BASE));
        cci::cci_get_broker().set_preset_cci_value(full_name + ".mem.size", cci::cci_value(VIRTIO_SIZE));
        cci::cci_get_broker().set_preset_cci_value(full_name + ".mem.relative_addresses", cci::cci_value(true));

        SCP_INFO(SCMOD) << "Creating VirtIO network device test with basic user-mode networking";

        // Create the VirtIO network device
        m_net_device = std::make_unique<virtio_mmio_net>("virtio_net", m_qemu_inst);

        // Connect device to router
        m_router.add_target(m_net_device->socket, VIRTIO_BASE, VIRTIO_SIZE);
        m_initiator.socket.bind(m_router.target_socket);

        // Start iterative network device test with resets
        SC_THREAD(run_iterative_tests);
    }

    void run_iterative_tests()
    {
        SCP_INFO(SCMOD) << "Starting iterative VirtIO network device tests with device resets (" << NUM_ITERATIONS
                        << " iterations)";

        for (m_current_iteration = 1; m_current_iteration <= NUM_ITERATIONS; m_current_iteration++) {
            SCP_INFO(SCMOD) << "=== Iteration " << m_current_iteration << "/" << NUM_ITERATIONS << " ===";

            // Wait for device initialization (longer wait on first iteration)
            if (m_current_iteration == 1) {
                sc_core::wait(100, sc_core::SC_MS);
            } else {
                sc_core::wait(50, sc_core::SC_MS); // Shorter wait after reset
            }

            // Perform the device initialization test
            bool iteration_passed = perform_single_device_test();

            if (iteration_passed) {
                m_successful_iterations++;
                SCP_INFO(SCMOD) << "Iteration " << m_current_iteration << " PASSED";
            } else {
                SCP_ERR(SCMOD) << "Iteration " << m_current_iteration << " FAILED";
                // Continue with remaining iterations even if one fails
            }

            // Perform device reset before next iteration (except on last iteration)
            if (m_current_iteration < NUM_ITERATIONS) {
                SCP_INFO(SCMOD) << "Performing device reset before iteration " << (m_current_iteration + 1);
                perform_device_reset();
                sc_core::wait(100, sc_core::SC_MS); // Wait for reset to complete
            }
        }

        // Determine overall test result
        SCP_INFO(SCMOD) << "=== Test Summary ===";
        SCP_INFO(SCMOD) << "Successful iterations: " << m_successful_iterations << "/" << NUM_ITERATIONS;

        if (m_successful_iterations == NUM_ITERATIONS) {
            SCP_INFO(SCMOD) << "All iterations passed successfully!";
            m_test_passed = true;
        } else {
            SCP_ERR(SCMOD) << "Some iterations failed - test incomplete";
            m_test_passed = false;
        }

        m_test_completed = true;
        sc_core::sc_stop();
    }

    bool perform_single_device_test()
    {
        SCP_INFO(SCMOD) << "Starting VirtIO network device initialization test for iteration " << m_current_iteration;

        try {
            // Test 1: Verify VirtIO magic value
            SCP_INFO(SCMOD) << "Test 1: Verifying VirtIO magic value";
            uint32_t magic_value;
            if (!read_register(VIRTIO_MMIO_MAGIC, magic_value)) {
                SCP_ERR(SCMOD) << "Failed to read VirtIO magic register";
                return false;
            }

            if (magic_value != VIRTIO_MAGIC_VALUE) {
                SCP_ERR(SCMOD) << "Test 1 FAILED: Invalid magic value. Expected 0x" << std::hex << VIRTIO_MAGIC_VALUE
                               << ", got 0x" << magic_value;
                return false;
            }

            SCP_INFO(SCMOD) << "Test 1 PASSED: VirtIO magic value verified (0x" << std::hex << magic_value << ")";

            // Test 2: Verify VirtIO version
            SCP_INFO(SCMOD) << "Test 2: Verifying VirtIO version";
            uint32_t version;
            if (!read_register(VIRTIO_MMIO_VERSION, version)) {
                SCP_ERR(SCMOD) << "Failed to read VirtIO version register";
                return false;
            }

            if (version < 1 || version > 2) {
                SCP_ERR(SCMOD) << "Test 2 FAILED: Unsupported VirtIO version " << version;
                return false;
            }

            SCP_INFO(SCMOD) << "Test 2 PASSED: VirtIO version verified (" << version << ")";

            // Test 3: Verify device ID (network device = 1)
            SCP_INFO(SCMOD) << "Test 3: Verifying device ID for network device";
            uint32_t device_id;
            if (!read_register(VIRTIO_MMIO_DEVICE_ID, device_id)) {
                SCP_ERR(SCMOD) << "Failed to read VirtIO device ID register";
                return false;
            }

            if (device_id != VIRTIO_ID_NET) {
                SCP_ERR(SCMOD) << "Test 3 FAILED: Expected network device ID " << VIRTIO_ID_NET << ", got "
                               << device_id;
                return false;
            }

            SCP_INFO(SCMOD) << "Test 3 PASSED: Network device ID verified (" << device_id << ")";

            // Test 4: Verify vendor ID is non-zero
            SCP_INFO(SCMOD) << "Test 4: Verifying vendor ID";
            uint32_t vendor_id;
            if (!read_register(VIRTIO_MMIO_VENDOR_ID, vendor_id)) {
                SCP_ERR(SCMOD) << "Failed to read VirtIO vendor ID register";
                return false;
            }

            if (vendor_id == 0) {
                SCP_ERR(SCMOD) << "Test 4 FAILED: Vendor ID is zero";
                return false;
            }

            SCP_INFO(SCMOD) << "Test 4 PASSED: Vendor ID verified (0x" << std::hex << vendor_id << ")";

            // Test 5: Basic device initialization sequence
            SCP_INFO(SCMOD) << "Test 5: Testing basic device initialization sequence";

            // Reset the device
            if (!write_register(VIRTIO_MMIO_STATUS, 0)) {
                SCP_ERR(SCMOD) << "Failed to reset device status";
                return false;
            }

            // Acknowledge device
            if (!write_register(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE)) {
                SCP_ERR(SCMOD) << "Failed to acknowledge device";
                return false;
            }

            // Set driver status
            if (!write_register(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER)) {
                SCP_ERR(SCMOD) << "Failed to set driver status";
                return false;
            }

            // Read device features
            uint32_t device_features;
            if (!read_register(VIRTIO_MMIO_DEVICE_FEATURES, device_features)) {
                SCP_ERR(SCMOD) << "Failed to read device features";
                return false;
            }

            SCP_INFO(SCMOD) << "Device features: 0x" << std::hex << device_features;

            // Accept basic features (simplified - just accept what device offers)
            if (!write_register(VIRTIO_MMIO_DRIVER_FEATURES, device_features)) {
                SCP_ERR(SCMOD) << "Failed to write driver features";
                return false;
            }

            // Set features OK
            if (!write_register(VIRTIO_MMIO_STATUS,
                                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK)) {
                SCP_ERR(SCMOD) << "Failed to set features OK status";
                return false;
            }

            // Verify status
            uint32_t final_status;
            if (!read_register(VIRTIO_MMIO_STATUS, final_status)) {
                SCP_ERR(SCMOD) << "Failed to read final device status";
                return false;
            }

            if ((final_status & VIRTIO_STATUS_FEATURES_OK) == 0) {
                SCP_ERR(SCMOD) << "Test 5 FAILED: Device rejected feature negotiation";
                return false;
            }

            SCP_INFO(SCMOD) << "Test 5 PASSED: Device initialization sequence completed successfully";

            SCP_INFO(SCMOD) << "All VirtIO network device tests passed for iteration " << m_current_iteration;
            return true;

        } catch (const std::exception& e) {
            SCP_ERR(SCMOD) << "Exception during VirtIO network device testing in iteration " << m_current_iteration
                           << ": " << e.what();
            return false;
        }
    }

    bool read_register(uint64_t offset, uint32_t& value)
    {
        try {
            uint64_t address = VIRTIO_BASE + offset;
            tlm::tlm_response_status status = m_initiator.do_read(address, value);
            return status == tlm::TLM_OK_RESPONSE;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool write_register(uint64_t offset, uint32_t value)
    {
        try {
            uint64_t address = VIRTIO_BASE + offset;
            tlm::tlm_response_status status = m_initiator.do_write(address, value);
            return status == tlm::TLM_OK_RESPONSE;
        } catch (const std::exception&) {
            return false;
        }
    }

    void perform_device_reset()
    {
        SCP_INFO(SCMOD) << "Initiating device reset via QEMU instance";

        try {
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
        TEST_ASSERT(m_successful_iterations == NUM_ITERATIONS);

        SCP_INFO(SCMOD) << "Iterative VirtIO network MMIO test with device resets completed successfully";
        SCP_INFO(SCMOD) << "Successfully completed " << m_successful_iterations << "/" << NUM_ITERATIONS
                        << " iterations";
    }
};

int sc_main(int argc, char* argv[]) { return run_testbench<VirtioMmioNetTest>(argc, argv); }
