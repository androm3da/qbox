/*
 * This file is part of libqbox
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: GreenSocs 2021
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <systemc>

#include <cstdio>
#include <vector>
#include <deque>
#include <fstream>
#include <iostream>

#include "test/cpu.h"
#include "test/tester/dmi_soak.h"

#include "hexagon.h"
#include "qemu-instance.h"

/*
 * Hexagon DMI async invalidation test.
 *
 * In this test, all CPUs share the same DMI region. Each CPU does some
 * read/modify/write to its dedicated 64-bits memory area, and at some point
 * invalidate the whole region.
 *
 * This test is quite stressful for the whole DMI sub-system as invalidations
 * are very common and come from every CPU randomly. Each CPU checks that the
 * value it reads from memory is the one it expects, and at the end, the test
 * checks that all dedicated memory areas contain the final value (corresponding
 * to the number of read/modify/write operations the CPUs did).
 */
class CpuHexagonDmiAsyncInvalTest : public CpuTestBench<qemu_cpu_hexagon, CpuTesterDmiSoak>
{
public:
    static constexpr uint64_t NUM_WRITES = 512;

private:
    /*
     * Decrease the number of write per CPU when the number of CPUs increases
     * so that the test does not last forever.
     */
    uint64_t m_num_write_per_cpu;
    std::thread m_thread;
    bool running = 0;

    /*
     * Load Hexagon firmware from binary file compiled with LLVM tools.
     * The binary contains the constants at the end that need to be patched.
     */
    void load_firmware_binary(uint32_t dmi_addr, uint32_t mmio_addr, uint32_t num_writes)
    {
#ifdef FIRMWARE_BIN_PATH
        // Load the compiled firmware binary
        const char* firmware_path = FIRMWARE_BIN_PATH;
        std::ifstream file(firmware_path, std::ios::binary | std::ios::ate);

        if (!file.is_open()) {
            SCP_FATAL(SCMOD) << "Failed to open Hexagon firmware file: " << firmware_path;
            TEST_ASSERT(false);
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> firmware_data(size);
        if (!file.read(reinterpret_cast<char*>(firmware_data.data()), size)) {
            SCP_FATAL(SCMOD) << "Failed to read Hexagon firmware file: " << firmware_path;
            TEST_ASSERT(false);
        }

        // Patch the constants in the binary (last 12 bytes: dmi_addr, mmio_addr, num_writes)
        if (size >= 12) {
            uint32_t* dmi_addr_ptr = reinterpret_cast<uint32_t*>(firmware_data.data() + size - 12);
            uint32_t* mmio_addr_ptr = reinterpret_cast<uint32_t*>(firmware_data.data() + size - 8);
            uint32_t* num_writes_ptr = reinterpret_cast<uint32_t*>(firmware_data.data() + size - 4);

            SCP_INFO(SCMOD) << "Before patching: dmi=0x" << std::hex << *dmi_addr_ptr << ", mmio=0x" << *mmio_addr_ptr
                            << ", num_writes=0x" << *num_writes_ptr;

            *dmi_addr_ptr = dmi_addr;
            *mmio_addr_ptr = mmio_addr;
            *num_writes_ptr = num_writes;

            SCP_INFO(SCMOD) << "After patching: dmi=0x" << std::hex << *dmi_addr_ptr << ", mmio=0x" << *mmio_addr_ptr
                            << ", num_writes=0x" << *num_writes_ptr;
        }

        // Load firmware directly into memory
        SCP_INFO(SCMOD) << "Loading Hexagon DMI test firmware at 0x" << std::hex << MEM_ADDR << ", size=" << std::dec
                        << size << " bytes";
        m_mem.load.ptr_load(firmware_data.data(), MEM_ADDR, size);
#else
        SCP_FATAL(SCMOD) << "FIRMWARE_BIN_PATH not defined - LLVM firmware compilation failed";
        TEST_ASSERT(false);
#endif
    }

public:
    CpuHexagonDmiAsyncInvalTest(const sc_core::sc_module_name& n): CpuTestBench<qemu_cpu_hexagon, CpuTesterDmiSoak>(n)
    {
        SCP_DEBUG(SCMOD) << "CpuHexagonDmiAsyncInvalTest constructor";
        m_num_write_per_cpu = NUM_WRITES / p_num_cpu;

        for (int i = 0; i < m_cpus.size(); i++) {
            auto& cpu = m_cpus[i];
            cpu.p_hexagon_num_threads = m_cpus.size();
            cpu.p_start_powered_off = (i != 0);
            cpu.p_exec_start_addr = 0x0;
        }

        // Load Hexagon binary firmware compiled with LLVM tools
        load_firmware_binary(static_cast<uint32_t>(CpuTesterDmiSoak::DMI_ADDR),
                             static_cast<uint32_t>(CpuTesterDmiSoak::MMIO_ADDR),
                             static_cast<uint32_t>(m_num_write_per_cpu));
    }

    virtual void start_of_simulation() override
    {
        running = true;
        m_thread = std::thread([&]() { inval(); });
    }
    void inval()
    {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uint64_t l = CpuTesterDmiSoak::DMI_SIZE;
            uint64_t s = (std::rand() + 1u) % l;
            l -= s;
            uint64_t e = s + ((std::rand() + 1u) % l);
            if (running) {
                SCP_INFO(SCMOD) << "INVALIDATING";
                m_tester.dmi_invalidate(s, e);
                SCP_INFO(SCMOD) << "Invalidation done";
            }
        }
    }

    virtual ~CpuHexagonDmiAsyncInvalTest() {}

    virtual void mmio_write(int id, uint64_t addr, uint64_t data, size_t len) override
    {
        int cpuid = addr >> 3;

        if (id != CpuTesterDmiSoak::SOCKET_MMIO) {
            SCP_INFO(SCMOD) << "NON DMI write data: 0x" << std::hex << data << ", len: 0x" << len;

            return;
        }

        SCP_INFO(SCMOD) << "cpu_" << cpuid << " write at 0x" << std::hex << addr << " data:0x" << data << ", len: 0x"
                        << len;
        if (data == 0) {
            SCP_INFO(SCMOD) << "cpu_" << cpuid << " DONE";
        }
        TEST_ASSERT(data != -1);
        TEST_ASSERT(data != -2);
    }

    virtual uint64_t mmio_read(int id, uint64_t addr, size_t len) override
    {
        int cpuid = addr >> 3;

        /* No read on the control socket */
        TEST_ASSERT(id == CpuTesterDmiSoak::SOCKET_DMI);
        SCP_INFO(SCMOD) << "CPU NON DMI read at 0x" << std::hex << addr;

        /* The return value is ignored by the tester */
        return 0;
    }

    virtual bool dmi_request(int id, uint64_t addr, size_t len, tlm::tlm_dmi& ret) override
    {
        SCP_INFO(SCMOD) << "DMI request at " << addr << ", len: " << len;
        return true;
    }

    virtual void end_of_simulation() override
    {
        CpuTestBench<qemu_cpu_hexagon, CpuTesterDmiSoak>::end_of_simulation();
        running = false;
        m_thread.join();
    }
};

int sc_main(int argc, char* argv[]) { return run_testbench<CpuHexagonDmiAsyncInvalTest>(argc, argv); }
