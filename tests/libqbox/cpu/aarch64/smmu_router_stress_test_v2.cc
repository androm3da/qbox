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
#include <queue>
#include <scp/report.h>
#include <cci/utils/broker.h>
#include <libgsutils.h>
#include <cciutils.h>
#include <argparser.h>
#include <keystone/keystone.h>
#include <tlm_utils/tlm_quantumkeeper.h>
#include "cci/cfg/cci_broker_if.h"
#include "test/cpu.h"
#include "test/tester/mmio.h"
#include "cortex-a53.h"
#include "qemu-instance.h"
#include <router.h>
#include <gs_memory.h>
#include <smmu500.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <pass.h>

#define USE_PASS_FOR_IDENTITY
/*
 * Helper module for memory access with proper routing
 */
class MemoryAccessor : public sc_core::sc_module
{
public:
    tlm_utils::simple_initiator_socket<MemoryAccessor> socket;

    MemoryAccessor(sc_core::sc_module_name name): sc_core::sc_module(name), socket("socket") {}

    void write_memory(uint64_t addr, uint64_t value)
    {
        tlm::tlm_generic_payload txn;
        txn.set_command(tlm::TLM_WRITE_COMMAND);
        txn.set_address(addr);
        txn.set_data_ptr(reinterpret_cast<unsigned char*>(&value));
        txn.set_data_length(sizeof(value));
        txn.set_streaming_width(sizeof(value));
        txn.set_byte_enable_length(0);
        txn.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        socket->b_transport(txn, delay);
    }

    uint64_t read_memory(uint64_t addr)
    {
        uint64_t value = 0;
        tlm::tlm_generic_payload txn;
        txn.set_command(tlm::TLM_READ_COMMAND);
        txn.set_address(addr);
        txn.set_data_ptr(reinterpret_cast<unsigned char*>(&value));
        txn.set_data_length(sizeof(value));
        txn.set_streaming_width(sizeof(value));
        txn.set_byte_enable_length(0);
        txn.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        socket->b_transport(txn, delay);
        return value;
    }

    void write_register(uint32_t addr, uint32_t value)
    {
        tlm::tlm_generic_payload txn;
        txn.set_command(tlm::TLM_WRITE_COMMAND);
        txn.set_address(addr);
        txn.set_data_ptr(reinterpret_cast<unsigned char*>(&value));
        txn.set_data_length(4);
        txn.set_streaming_width(4);
        txn.set_byte_enable_length(0);
        txn.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        // Use b_transport for register access to ensure proper logging
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        socket->b_transport(txn, delay);
    }

    uint32_t read_register(uint32_t addr)
    {
        uint32_t value = 0;
        tlm::tlm_generic_payload txn;
        txn.set_command(tlm::TLM_READ_COMMAND);
        txn.set_address(addr);
        txn.set_data_ptr(reinterpret_cast<unsigned char*>(&value));
        txn.set_data_length(4);
        txn.set_streaming_width(4);
        txn.set_byte_enable_length(0);
        txn.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        // Use b_transport for register access to ensure proper logging
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        socket->b_transport(txn, delay);
        return value;
    }
};

/*
 * SMMU Stress Test V2 - Tester-Controlled Architecture
 *
 * NEW APPROACH:
 * - No sync memory - all coordination through tester MMIO registers
 * - Tester handles all SMMU configuration directly
 * - CPUs request regions via MMIO writes, poll for readiness via MMIO reads
 * - Simplified protocol: REQUEST -> POLL -> WORK -> COMPLETE
 *
 * PROTOCOL:
 * CPU -> Tester: "I want to fill a region" (write to MMIO)
 * Tester: Configures SMMU mapping for that CPU to an available region
 * CPU: Polls tester register until ready
 * CPU: Fills region with pattern
 * CPU -> Tester: "Fill complete" (write to MMIO)
 *
 * Same for checking regions.
 */

class SMMUTesterController : public sc_core::sc_module
{
public:
    // MMIO Register Layout (per CPU)
    static constexpr uint64_t REG_SIZE_PER_CPU = 0x100;
    static constexpr uint64_t REG_REQUEST = 0x00;    // CPU writes: 1=fill_request, 2=check_request
    static constexpr uint64_t REG_STATUS = 0x08;     // CPU reads: 0=busy, 1=ready, 2=complete
    static constexpr uint64_t REG_REGION_ID = 0x10;  // CPU reads: assigned region ID
    static constexpr uint64_t REG_COMPLETE = 0x18;   // CPU writes: 1=fill_done, 2=check_done
    static constexpr uint64_t REG_ITERATIONS = 0x20; // CPU reads: current iteration count
    static constexpr uint64_t REG_DEBUG = 0x28;      // CPU writes: debug messages
    static constexpr uint64_t REG_DEBUG_DATA = 0x30; // CPU writes: debug messages

    // Request types
    enum RequestType { NONE = 0, FILL_REQUEST = 1, CHECK_REQUEST = 2 };
    enum StatusType { BUSY = 0, READY = 1, COMPLETE = 2 };
    enum CompleteType { FILL_DONE = 1, CHECK_DONE = 2 };

    struct CPUState {
        RequestType current_request = NONE;
        StatusType status = BUSY;
        uint32_t assigned_region = 0xFFFFFFFF;
        uint32_t iteration_count = 0;
        bool is_working = false;
    };

    tlm_utils::simple_target_socket<SMMUTesterController> socket;

    // Reference to parent test bench for SMMU control
    class CpuArmCortexA53SMMUStressTestV2* m_parent;

    std::vector<CPUState> m_cpu_states;
    std::queue<uint32_t> m_available_regions;
    std::queue<uint32_t> m_regions_to_check;
    uint32_t m_num_cpus;
    uint32_t m_num_regions;
    uint32_t m_global_iterations;
    uint32_t m_target_iterations;

    SMMUTesterController(sc_core::sc_module_name name, uint32_t num_cpus, uint32_t num_regions,
                         uint32_t target_iterations)
        : sc_core::sc_module(name)
        , socket("socket")
        , m_parent(nullptr)
        , m_num_cpus(num_cpus)
        , m_num_regions(num_regions)
        , m_global_iterations(0)
        , m_target_iterations(target_iterations)
    {
        socket.register_b_transport(this, &SMMUTesterController::b_transport);
        socket.register_transport_dbg(this, &SMMUTesterController::transport_dbg);

        // Initialize CPU states
        m_cpu_states.resize(num_cpus);

        // Initialize available regions queue
        for (uint32_t i = 0; i < num_regions; ++i) {
            m_available_regions.push(i);
        }

        SCP_INFO(SCMOD) << "SMMU Tester Controller initialized: " << num_cpus << " CPUs, " << num_regions
                        << " regions, target " << target_iterations << " iterations";
    }

    void set_parent(class CpuArmCortexA53SMMUStressTestV2* parent) { m_parent = parent; }

    virtual void b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay)
    {
        uint64_t addr = trans.get_address();
        uint32_t cpu_id = static_cast<uint32_t>(addr / REG_SIZE_PER_CPU);
        uint64_t reg_offset = addr % REG_SIZE_PER_CPU;

        if (cpu_id >= m_num_cpus) {
            trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
            return;
        }

        if (trans.get_command() == tlm::TLM_WRITE_COMMAND) {
            handle_write(cpu_id, reg_offset, trans);
        } else if (trans.get_command() == tlm::TLM_READ_COMMAND) {
            handle_read(cpu_id, reg_offset, trans);
        }

        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    virtual unsigned int transport_dbg(tlm::tlm_generic_payload& trans)
    {
        // For debug transport, just call b_transport with zero delay
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        b_transport(trans, delay);
        return trans.get_data_length();
    }

private:
    void handle_write(uint32_t cpu_id, uint64_t reg_offset, tlm::tlm_generic_payload& trans)
    {
        uint64_t data = *reinterpret_cast<uint64_t*>(trans.get_data_ptr());
        CPUState& cpu = m_cpu_states[cpu_id];

        switch (reg_offset) {
        case REG_REQUEST:
            handle_request(cpu_id, static_cast<RequestType>(data));
            break;

        case REG_COMPLETE:
            // moved out-of-class after test bench definition to avoid incomplete type error
            handle_complete(cpu_id, static_cast<CompleteType>(data));
            break;

        case REG_DEBUG:
            handle_debug(cpu_id, data);
            break;

        case REG_DEBUG_DATA:
            SCP_DEBUG(SCMOD) << "CPU " << cpu_id << " debug: 0x" << std::hex << data;
            break;

        default:
            SCP_WARN(SCMOD) << "CPU " << cpu_id << " write to unknown register 0x" << std::hex << reg_offset;
            break;
        }
    }

    // move handle_complete declaration only
    void handle_complete(uint32_t cpu_id, CompleteType complete);

    void handle_read(uint32_t cpu_id, uint64_t reg_offset, tlm::tlm_generic_payload& trans)
    {
        uint64_t data = 0;
        CPUState& cpu = m_cpu_states[cpu_id];

        switch (reg_offset) {
        case REG_STATUS:
            data = static_cast<uint64_t>(cpu.status);
            break;

        case REG_REGION_ID:
            data = cpu.assigned_region;
            break;

        case REG_ITERATIONS:
            data = m_global_iterations;
            break;

        default:
            SCP_WARN(SCMOD) << "CPU " << cpu_id << " read from unknown register 0x" << std::hex << reg_offset;
            break;
        }

        *reinterpret_cast<uint64_t*>(trans.get_data_ptr()) = data;
    }

    void handle_request(uint32_t cpu_id, RequestType request)
    {
        CPUState& cpu = m_cpu_states[cpu_id];

        SCP_INFO(SCMOD) << "CPU " << cpu_id << " request: " << (request == FILL_REQUEST ? "FILL" : "CHECK");

        if (request == FILL_REQUEST) {
            if (!m_available_regions.empty()) {
                uint32_t region = m_available_regions.front();
                m_available_regions.pop();

                cpu.current_request = FILL_REQUEST;
                cpu.assigned_region = region;
                cpu.is_working = true;

                SCP_INFO(SCMOD) << "DEBUG: handle_request(FILL) assigning CPU " << cpu_id << " to region " << region
                                << ". Calling configure_smmu_mapping()";
                // Configure SMMU mapping for this CPU to this region
                configure_smmu_mapping(cpu_id, region);

                // Mark as ready
                cpu.status = READY;

                SCP_INFO(SCMOD) << "CPU " << cpu_id << " assigned region " << region << " for filling";
            } else {
                cpu.status = BUSY; // No regions available
                SCP_DEBUG(SCMOD) << "CPU " << cpu_id << " fill request - no regions available";
            }
        } else if (request == CHECK_REQUEST) {
            if (!m_regions_to_check.empty()) {
                uint32_t region = m_regions_to_check.front();
                m_regions_to_check.pop();

                cpu.current_request = CHECK_REQUEST;
                cpu.assigned_region = region;
                cpu.is_working = true;

                // Configure SMMU mapping for this CPU to this region
                configure_smmu_mapping(cpu_id, region);

                // Mark as ready
                cpu.status = READY;

                SCP_INFO(SCMOD) << "CPU " << cpu_id << " assigned region " << region << " for checking";
            } else {
                cpu.status = BUSY; // No regions to check
                SCP_DEBUG(SCMOD) << "CPU " << cpu_id << " check request - no regions to check";
            }
        }
    }

    // Definition moved after CpuArmCortexA53SMMUStressTestV2 below

    void handle_debug(uint32_t cpu_id, uint64_t data)
    {
        // Decode debug messages
        uint32_t msg_type = (data & 0xF000) >> 12;
        uint32_t msg_data = (data & 0x0FFF);

        switch (msg_type) {
        case 0x1:
            SCP_INFO(SCMOD) << "CPU " << cpu_id << " started up";
            break;
        case 0x2:
            SCP_INFO(SCMOD) << "CPU " << cpu_id << " entering main loop, iteration " << msg_data;
            break;
        case 0x3:
            SCP_INFO(SCMOD) << "CPU " << cpu_id << " polling for readiness";
            break;
        case 0x4:
            SCP_INFO(SCMOD) << "CPU " << cpu_id << " starting work on region " << msg_data;
            break;
        case 0x5:
            SCP_INFO(SCMOD) << "CPU " << cpu_id << " pattern verification success";
            break;
        case 0x6:
            SCP_INFO(SCMOD) << "CPU " << cpu_id << " DEBUG: Starting fill for region " << msg_data;
            break;
        case 0x7:
            SCP_INFO(SCMOD) << "CPU " << cpu_id << " DEBUG: Fill write #" << msg_data << " (first 4 writes logged)";
            break;
        case 0x8:
            SCP_INFO(SCMOD) << "CPU " << cpu_id << " DEBUG: Completed fill for region " << msg_data;
            break;
        case 0xD:
            SCP_WARN(SCMOD) << "CPU " << cpu_id << " pattern verification FAILED";
            break;
        case 0xE:
            SCP_FATAL(SCMOD) << "🚨 DIAGNOSTIC ERROR: CPU " << cpu_id
                             << " encountered fatal error - jumping to diagnostic handler at 0x200";
            SCP_FATAL(SCMOD) << "This indicates a translation fault or other critical issue during CPU execution";
            sc_core::sc_stop();
            break;
        default:
            SCP_DEBUG(SCMOD) << "CPU " << cpu_id << " debug: 0x" << std::hex << data;
            break;
        }
    }

    void configure_smmu_mapping(uint32_t cpu_id, uint32_t region_id);
    void unmap_cpu_region(uint32_t cpu_id);

    // NEW METHODS: Tester directly verifies region patterns in physical memory
    bool verify_region_pattern(uint32_t cpu_id, uint32_t region_id);
    void clear_region_pattern(uint32_t region_id);
};

class CpuArmCortexA53SMMUStressTestV2 : public TestBench, public CpuTesterCallbackIface
{
public:
    static constexpr uint16_t MAX_ITERATIONS = 1000; // Limited to 16-bit for ARM64 movz instruction
    static constexpr uint64_t PATTERN_SIZE = 16;
    sc_core::sc_time TEST_DURATION = sc_core::sc_time(30, sc_core::SC_SEC);

    // Memory layout - REORGANIZED: Everything in first 1GB with 1:1 mapping
    static constexpr uint64_t MEM_ADDR = 0x0;                  // FIRMWARE: 0x00000000 - 0x0003FFFF
    static constexpr size_t MEM_SIZE = 256 * 1024;             // 256KB for firmware
    static constexpr uint64_t BOOT_ADDR = 0x0;                 // BOOT: 0x00000000 - tiny boot loader
    static constexpr uint64_t DIAGNOSTIC_ADDR = 0x200;         // DIAGNOSTIC: 0x00000200 - error handler
    static constexpr uint64_t MAIN_FIRMWARE_ADDR = 0x1000;     // MAIN_FIRMWARE: 0x00001000 - main test logic
    static constexpr uint64_t TESTER_ADDR = 0x40000;           // TESTER: 0x00040000 - 0x0004FFFF
    static constexpr uint64_t TESTER_SIZE = 0x10000;           // 64KB for tester
    static constexpr uint64_t SMMU_REG_ADDR = 0x50000;         // SMMU: 0x00050000 - 0x0006FFFF
    static constexpr uint64_t SMMU_REG_SIZE = 0x20000;         // 128KB for SMMU registers
    static constexpr uint64_t MAIN_MEM_ADDR = 0x10000000;      // MEMORY: 0x10000000 - 0x1FFFFFFF
    static constexpr size_t MAIN_MEM_SIZE = 256 * 1024 * 1024; // 256MB for main memory

    // Virtual and physical memory regions - Use high virtual address >32-bit
    static constexpr uint64_t VIRTUAL_TEST_ADDR = 0x300000000ULL; // 12GB virtual address
    static constexpr uint64_t
        PAGE_TABLE_BASE = 0x10000000; // Page tables: 0x10000000 - 0x1007FFFF (512KB for up to 32 CPUs)
    static constexpr uint64_t REGION_BASE = 0x10080000; // Test regions: 0x10080000+ in 4KB blocks (after page tables)
    static constexpr uint64_t REGION_SIZE = 0x1000;     // 4KB regions (one page each)
    static constexpr uint64_t PAGE_SIZE = 0x1000;       // 4KB pages

protected:
    // CCI parameters
    cci::cci_param<int> p_num_cpu;
    cci::cci_param<int> p_quantum_ns;

    // QEMU instances and CPUs
    QemuInstanceManager m_inst_manager;
    QemuInstance m_inst_a;
    QemuInstance m_inst_b;
    bool ab = false;
    sc_core::sc_vector<cpu_arm_cortexA53> m_cpus;

    // Memory components
    gs::gs_memory<> m_mem;
    gs::gs_memory<> m_main_mem;

    // SMMU components
    gs::smmu500<> m_smmu;
#ifdef USE_PASS_FOR_IDENTITY
    std::vector<gs::pass<>*> m_pass_identity; // Pass-through for identity traffic
#else
    std::vector<gs::smmu500_tbu<>*> m_tbus_identity; // TBUs for identity traffic
#endif
    std::vector<gs::smmu500_tbu<>*> m_tbus_high_va; // TBUs for high VA traffic

    // Routers
    gs::router<> m_global_router;
    std::vector<gs::router<>*> m_cpu_routers; // Per-CPU routers

    // Tester controller
    SMMUTesterController m_tester_controller;

    uint32_t m_num_regions;
    std::vector<uint32_t> m_cpu_to_region;

public:
    // Memory accessor for proper routing - made public for tester access
    MemoryAccessor m_memory_accessor;

protected:
    void set_firmware(const char* assembly, uint64_t addr = 0)
    {
        ks_engine* ks;
        ks_err err;
        size_t size, count;
        uint8_t* fw;

        err = ks_open(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN, &ks);

        if (err != KS_ERR_OK) {
            SCP_FATAL(SCMOD) << "Unable to initialize keystone";
        }

        if (ks_asm(ks, assembly, addr, &fw, &size, &count) != KS_ERR_OK || size == 0) {
            std::cerr << assembly << "\n";
            std::cerr << "errno: " << ks_errno(ks) << "\n";
            std::cerr << "error: " << ks_strerror(ks_errno(ks)) << "\n";
            SCP_INFO() << assembly;
            TEST_FAIL("Unable to assemble the test firmware\n");
        }

        m_mem.load.ptr_load(fw, addr, size);

        ks_free(fw);
        ks_close(ks);
    }

public:
    CpuArmCortexA53SMMUStressTestV2(const sc_core::sc_module_name& n)
        : TestBench(n)
        , p_num_cpu("num_cpu", 1, "Number of CPUs to instantiate in the test")
        , p_quantum_ns("quantum_ns", 1000000, "Value of the global TLM-2.0 quantum in ns")
        , m_inst_a("inst_a", &m_inst_manager, cpu_arm_cortexA53::ARCH)
        , m_inst_b("inst_b", &m_inst_manager, cpu_arm_cortexA53::ARCH)
        , m_cpus("cpu", p_num_cpu,
                 [this](const char* n, int i) {
                     ab = !ab;
                     return new cpu_arm_cortexA53(n, ab ? m_inst_a : m_inst_b);
                 })
        , m_mem("mem", MEM_SIZE)
        , m_main_mem("main_mem", MAIN_MEM_SIZE)
        , m_smmu("smmu")
        , m_global_router("global_router")
        , m_tester_controller("tester_controller", p_num_cpu.get_value(),
                              std::max(3u, static_cast<uint32_t>(p_num_cpu.get_value() * 3)), MAX_ITERATIONS)
        , m_memory_accessor("memory_accessor")
    {
        using tlm_utils::tlm_quantumkeeper;

        // Set up ARM-specific CPU configuration
        int i = 0;
        for (cpu_arm_cortexA53& cpu : m_cpus) {
            cpu.p_mp_affinity = i++;
            cpu.p_has_el3 = false;
        }

        sc_core::sc_time global_quantum(p_quantum_ns, sc_core::SC_NS);
        tlm_quantumkeeper::set_global_quantum(global_quantum);

        m_num_regions = std::max(3u, static_cast<uint32_t>(p_num_cpu.get_value() * 3));

        SCP_INFO(SCMOD) << "Creating SMMU Stress Test V2 with " << p_num_cpu.get_value() << " CPUs, " << m_num_regions
                        << " regions";
        SCP_INFO(SCMOD) << "NEW ARCHITECTURE: Tester-controlled SMMU configuration";

        // Configure SMMU

        // Bind memory_accessor.socket to router BEFORE any accesses!
        m_global_router.add_initiator(m_memory_accessor.socket);

        m_smmu.p_num_tbu = p_num_cpu.get_value();
        m_smmu.p_num_cb = p_num_cpu.get_value() * 2; // Need 2 CBs per CPU: identity + high VA
        m_smmu.p_num_smr = 64;                       // Increased from 32 to support up to 32 CPUs (each needs 2 SMRs)
        m_smmu.p_num_pages = std::max(
            16u, static_cast<uint32_t>(p_num_cpu.get_value() * 2)); // Ensure enough pages for all CBs
        SCP_INFO(SCMOD) << "SMMU500 instantiated: p_num_cb=" << m_smmu.p_num_cb << ", p_num_smr=" << m_smmu.p_num_smr
                        << ", p_num_pages=" << m_smmu.p_num_pages;

        // Create TBU instances - 2 TBUs per CPU (identity + high VA)
        uint32_t num_cpus = p_num_cpu.get_value();
#ifdef USE_PASS_FOR_IDENTITY
        m_pass_identity.resize(num_cpus);
#else
        m_tbus_identity.resize(num_cpus);
#endif
        m_tbus_high_va.resize(num_cpus);

        for (uint32_t i = 0; i < num_cpus; ++i) {
#ifdef USE_PASS_FOR_IDENTITY
            // Identity pass-through for each CPU - bypasses SMMU for identity traffic
            char pass_identity_name[32];
            std::snprintf(pass_identity_name, sizeof(pass_identity_name), "pass_identity_%d", i);
            m_pass_identity[i] = new gs::pass<>(pass_identity_name);
            SCP_INFO(SCMOD) << "Identity pass-through constructed: CPU" << i
                            << " (bypassing SMMU for identity traffic)";
#else
            // Identity TBU for each CPU - ALL share StreamID 0 → CB0
            char tbu_identity_name[32];
            std::snprintf(tbu_identity_name, sizeof(tbu_identity_name), "tbu_identity_%d", i);
            m_tbus_identity[i] = new gs::smmu500_tbu<>(tbu_identity_name, &m_smmu);
            m_tbus_identity[i]->p_topology_id = 0; // All identity TBUs share StreamID 0
            m_tbus_identity[i]->p_topology_id.set_value(0);
            SCP_INFO(SCMOD) << "Identity TBU constructed: CPU" << i << " topology_id=0"
                            << " (StreamID 0 → CB0 shared identity)";
#endif

            // High VA TBU for each CPU - unique StreamID per CPU
            char tbu_high_va_name[32];
            std::snprintf(tbu_high_va_name, sizeof(tbu_high_va_name), "tbu_high_va_%d", i);
            m_tbus_high_va[i] = new gs::smmu500_tbu<>(tbu_high_va_name, &m_smmu);
            uint32_t high_va_topology_id = i + 1; // StreamID 1,2,3... for high VA
            m_tbus_high_va[i]->p_topology_id = high_va_topology_id;
            m_tbus_high_va[i]->p_topology_id.set_value(high_va_topology_id);
            SCP_INFO(SCMOD) << "High VA TBU constructed: CPU" << i << " topology_id=" << high_va_topology_id
                            << " (StreamID " << high_va_topology_id << " → CB" << high_va_topology_id << ")";
        }

        // Create per-CPU routers
        m_cpu_routers.resize(num_cpus);
        for (uint32_t i = 0; i < num_cpus; ++i) {
            char router_name[32];
            std::snprintf(router_name, sizeof(router_name), "cpu_router_%d", i);
            m_cpu_routers[i] = new gs::router<>(router_name);
            SCP_INFO(SCMOD) << "Per-CPU router constructed: " << router_name;
        }

        // Initialize CPU to region mapping
        m_cpu_to_region.resize(p_num_cpu.get_value(), 0xFFFFFFFF);

        // Set up tester controller parent reference
        m_tester_controller.set_parent(this);

        // NEW ARCHITECTURE: CPU -> Per-CPU Router -> Identity/High VA TBUs -> Global Router
        for (uint32_t i = 0; i < p_num_cpu.get_value(); ++i) {
            // Connect CPU to per-CPU router
            m_cpu_routers[i]->add_initiator(m_cpus[i].socket);

            // Configure per-CPU router address ranges
#ifdef USE_PASS_FOR_IDENTITY
            // Identity traffic (<0x300000000) -> Pass-through (bypasses SMMU)
            m_cpu_routers[i]->add_target(m_pass_identity[i]->target_socket, 0x0, 0x10000000ULL);
#else
            // Identity traffic (<0x300000000) -> Identity TBU
            m_cpu_routers[i]->add_target(m_tbus_identity[i]->upstream_socket, 0x0, 0x10000000ULL);
#endif

            // High VA traffic (>=0x300000000) -> High VA TBU
            m_cpu_routers[i]->add_target(m_tbus_high_va[i]->upstream_socket, 0x300000000ULL, 0x100000000ULL);

            SCP_INFO(SCMOD) << "🔍 ROUTING DEBUG: CPU " << i << " -> CPU_Router_" << i;
#ifdef USE_PASS_FOR_IDENTITY
            SCP_INFO(SCMOD) << "  - Identity range [0x0 - 0x10000000] -> Pass_Identity_" << i << " (bypassing SMMU)";
#else
            SCP_INFO(SCMOD) << "  - Identity range [0x0 - 0x10000000] -> Identity_TBU_" << i << " (StreamID 0)";
#endif
            SCP_INFO(SCMOD) << "  - High VA range [0x300000000 - 0x400000000] -> High_VA_TBU_" << i << " (StreamID "
                            << (i + 1) << ")";
#ifdef USE_PASS_FOR_IDENTITY
            SCP_INFO(SCMOD) << "  - Identity pass name: " << m_pass_identity[i]->name();
#else
            SCP_INFO(SCMOD) << "  - Identity TBU name: " << m_tbus_identity[i]->name();
#endif
            SCP_INFO(SCMOD) << "  - High VA TBU name: " << m_tbus_high_va[i]->name();
        }

        // Add components to global router
        m_global_router.add_target(m_mem.socket, MEM_ADDR, MEM_SIZE);

        // Add main memory as a single large region - SMMU will handle the translation
        m_global_router.add_target(m_main_mem.socket, MAIN_MEM_ADDR, MAIN_MEM_SIZE);

        m_global_router.add_target(m_smmu.socket, SMMU_REG_ADDR, SMMU_REG_SIZE);
        m_global_router.add_target(m_tester_controller.socket, TESTER_ADDR, TESTER_SIZE);

        // Connect TBU downstream sockets to global router
        for (uint32_t i = 0; i < p_num_cpu.get_value(); ++i) {
#ifdef USE_PASS_FOR_IDENTITY
            m_global_router.add_initiator(m_pass_identity[i]->initiator_socket);
#else
            m_global_router.add_initiator(m_tbus_identity[i]->downstream_socket);
#endif
            m_global_router.add_initiator(m_tbus_high_va[i]->downstream_socket);
        }

        // Connect SMMU DMA socket
        m_global_router.add_initiator(m_smmu.dma_socket);

        SCP_INFO(SCMOD) << "Memory layout:";
        SCP_INFO(SCMOD) << "  FIRMWARE: 0x" << std::hex << MEM_ADDR;
        SCP_INFO(SCMOD) << "  MAIN_MEM: 0x" << std::hex << MAIN_MEM_ADDR;
        SCP_INFO(SCMOD) << "  REGIONS: 0x" << std::hex << REGION_BASE;
        SCP_INFO(SCMOD) << "  SMMU_REG: 0x" << std::hex << SMMU_REG_ADDR;
        SCP_INFO(SCMOD) << "  TESTER: 0x" << std::hex << TESTER_ADDR;
        SCP_INFO(SCMOD) << "  VIRTUAL_TEST: 0x" << std::hex << VIRTUAL_TEST_ADDR;

        generate_tester_controlled_firmware();

        SC_THREAD(configure_test);
        //        SC_THREAD(timeout_monitor);
    }

    virtual ~CpuArmCortexA53SMMUStressTestV2()
    {
#ifdef USE_PASS_FOR_IDENTITY
        for (auto* pass : m_pass_identity) {
            delete pass;
        }
#else
        for (auto* tbu : m_tbus_identity) {
            delete tbu;
        }
#endif
        for (auto* tbu : m_tbus_high_va) {
            delete tbu;
        }
        for (auto* router : m_cpu_routers) {
            delete router;
        }
    }

    void configure_test()
    {
        wait(sc_core::sc_time(100, sc_core::SC_US));

        SCP_INFO(SCMOD) << "Configuring SMMU for tester-controlled operation";

        // First: clear CLIENTPD in SMMU_SCR0 (enable SMMU translation)
        write_smmu_register(SMMU_REG_ADDR + 0x0, 0x0);

        // Configure SMRs and S2CRs for NEW DUAL-TBU ARCHITECTURE
        // SHARED IDENTITY: All identity TBUs share StreamID 0 → CB0
        // PER-CPU HIGH VA: Each CPU gets unique StreamID for high VA → separate CBs

        // SMR[0]: StreamID 0 -> CB0 (SHARED identity mapping for ALL CPUs)
        uint32_t smr0_addr = SMMU_REG_ADDR + 0x800;
        uint32_t smr0_value = (1 << 31) | (0 << 16) | (0 << 0); // VALID=1, MASK=0, ID=0
        write_smmu_register(smr0_addr, smr0_value);

        uint32_t s2cr0_addr = SMMU_REG_ADDR + 0xc00;
        uint32_t s2cr0_value = (0x1 << 16) | (0 << 0); // TYPE=1, CBNDX=0
        write_smmu_register(s2cr0_addr, s2cr0_value);

        SCP_INFO(SCMOD) << "SMR[0]/S2CR[0]: StreamID=0 -> CB0 (SHARED identity for ALL CPUs)";

        // Configure SMRs for high VA TBUs (one per CPU)
        for (uint32_t cpu = 0; cpu < p_num_cpu.get_value(); ++cpu) {
            uint32_t high_va_stream_id = cpu + 1; // StreamID 1,2,3... for high VA
            uint32_t high_va_cb = cpu + 1;        // CB1,CB2,CB3... for high VA

            uint32_t smr_addr = SMMU_REG_ADDR + 0x800 + (high_va_stream_id * 4);
            uint32_t smr_value = (1 << 31) | (0 << 16) | (high_va_stream_id << 0); // VALID=1, MASK=0, ID=stream_id
            write_smmu_register(smr_addr, smr_value);

            uint32_t s2cr_addr = SMMU_REG_ADDR + 0xc00 + (high_va_stream_id * 4);
            uint32_t s2cr_value = (0x1 << 16) | (high_va_cb << 0); // TYPE=1, CBNDX=cb
            write_smmu_register(s2cr_addr, s2cr_value);

            SCP_INFO(SCMOD) << "SMR[" << high_va_stream_id << "]/S2CR[" << high_va_stream_id
                            << "]: StreamID=" << high_va_stream_id << " -> CB" << high_va_cb << " (CPU " << cpu
                            << " high VA)";
        }

        // NEW ARCHITECTURE SUMMARY
        SCP_INFO(SCMOD) << "SMMU StreamID Mapping Summary (NEW DUAL-TBU ARCHITECTURE):";
        SCP_INFO(SCMOD) << "  StreamID 0 -> CB0 (SHARED identity for ALL CPUs - MMU disabled)";
        for (uint32_t cpu = 0; cpu < p_num_cpu.get_value(); ++cpu) {
            SCP_INFO(SCMOD) << "  StreamID " << (cpu + 1) << " -> CB" << (cpu + 1) << " (CPU " << cpu
                            << " high VA - 4KB pages)";
        }
        SCP_INFO(SCMOD) << "ARCHITECTURE: Shared identity CB0 + per-CPU high VA CBs!";

        // Initialize context banks for NEW DUAL-TBU ARCHITECTURE
        // CB0: SHARED identity context bank for ALL CPUs
        setup_identity_context_bank(0); // Only CB0 for shared identity

        // CB1,CB2,CB3...: Per-CPU high VA context banks
        for (uint32_t cpu = 0; cpu < p_num_cpu.get_value(); ++cpu) {
            uint32_t high_va_cb = cpu + 1; // CB1,CB2,CB3... for high VA

            // CRITICAL FIX: Use single consolidated function to avoid conflicts
            // This replaces both setup_high_va_context_bank() and setup_complete_page_table_structure()
            setup_complete_high_va_context_bank(cpu, high_va_cb);

            SCP_INFO(SCMOD) << "Complete high VA context bank initialized for CPU " << cpu << " (CB" << high_va_cb
                            << ") - conflicts resolved";
        }

        SCP_INFO(SCMOD) << "SMMU configuration completed - ready for tester control";
        SCP_INFO(SCMOD) << "✅ CRITICAL FIX APPLIED: All CPUs now have complete page table structures";
    }

    void setup_identity_context_bank(uint32_t cpu)
    {
        // Set up identity context bank (CB0, CB1) with NO page tables - pure identity mapping
        uint32_t cb = cpu; // Identity CBs are CB0, CB1
        uint32_t cb_offset_words = ((16 + cb) * 4096) / 4;
        uint32_t cb_base = SMMU_REG_ADDR + (cb_offset_words * 4);

        // Set CBAR.TYPE = 1 (translation enabled)
        uint32_t cbar_addr = SMMU_REG_ADDR + 0x1000 + (cb * 4);
        write_smmu_register(cbar_addr, (1 << 16)); // TYPE=1

        // CRITICAL: For identity mapping, DISABLE MMU completely
        // This makes VA = PA directly without any page table walking
        write_smmu_register(cb_base + 0x0, 0x0); // SCTLR: MMU disabled (M=0)

        // Clear TTBR registers (not used when MMU disabled)
        write_smmu_register(cb_base + 0x20, 0x0);
        write_smmu_register(cb_base + 0x24, 0x0);

        // Set TCR to safe defaults (not used when MMU disabled)
        write_smmu_register(cb_base + 0x30, (1U << 31) | (25 << 0)); // EAE=1, T0SZ=25

        // Configure MAIR (not used when MMU disabled)
        write_smmu_register(cb_base + 0x38, 0xFF);
        write_smmu_register(cb_base + 0x3C, 0x0);

        SCP_INFO(SCMOD) << "Identity context bank CB" << cb << " set up for CPU " << cpu
                        << " with MMU DISABLED (pure identity mapping VA=PA)";
    }

    void setup_complete_high_va_context_bank(uint32_t cpu, uint32_t cb)
    {
        // CRITICAL FIX: Consolidated function to replace conflicting setup functions
        // This combines setup_high_va_context_bank() and setup_complete_page_table_structure()
        // to eliminate the conflicts that cause "bad descriptor" SMMU faults

        SCP_INFO(SCMOD) << "🔧 CONSOLIDATED SETUP: Setting up complete high VA context bank for CPU " << cpu << " (CB"
                        << cb << ") - resolving function conflicts";

        // Use consistent page table addressing throughout
        uint64_t page_table_offset = (cpu + 1) * PAGE_SIZE * 4; // CB1=+4 pages, CB2=+8 pages, CB3=+12 pages
        uint64_t page_table_addr = PAGE_TABLE_BASE + page_table_offset;

        uint64_t l0_table_addr = page_table_addr;
        uint64_t l1_table_addr = page_table_addr + PAGE_SIZE;
        uint64_t l2_table_addr = page_table_addr + (PAGE_SIZE * 2);
        uint64_t l3_table_addr = page_table_addr + (PAGE_SIZE * 3);

        SCP_INFO(SCMOD) << "  - Page table base: 0x" << std::hex << page_table_addr;
        SCP_INFO(SCMOD) << "  - L0 table: 0x" << std::hex << l0_table_addr;
        SCP_INFO(SCMOD) << "  - L1 table: 0x" << std::hex << l1_table_addr;
        SCP_INFO(SCMOD) << "  - L2 table: 0x" << std::hex << l2_table_addr;
        SCP_INFO(SCMOD) << "  - L3 table: 0x" << std::hex << l3_table_addr;

        // Set up complete page table structure
        // L0[0]: 0x00000000 - 0x7FFFFFFFFF (0-512GB) -> L1 table
        uint64_t l0_desc_0 = (l1_table_addr & ~0xFFFULL) | (1ULL << 10) | 0x3ULL;
        write_memory_64(l0_table_addr + (0 * 8), l0_desc_0);

        // CRITICAL FIX: TBU strips upper bits, so VA=0x300000000 becomes VA=0x0
        // We need L1[0] to map to the SAME L2 table as L1[12] for proper translation
        // This is NOT identity mapping - it's mapping the stripped high VA to the correct region

        // L1[0]: VA=0x0 (stripped from 0x300000000) -> L2 table (same as high VA mapping)
        uint64_t l1_desc_0 = (l2_table_addr & ~0xFFFULL) | (1ULL << 10) | 0x3ULL;
        write_memory_64(l1_table_addr + (0 * 8), l1_desc_0);

        SCP_INFO(SCMOD) << "  - L1[0]: VA=0x0 (TBU-stripped from 0x300000000) -> L2 table (same as high VA)";

        // L1[12]: VIRTUAL_TEST_ADDR range -> L2 table (for high VA mapping)
        uint32_t l1_index = (VIRTUAL_TEST_ADDR >> 30) & 0x1FF;
        uint64_t l1_desc_high = (l2_table_addr & ~0xFFFULL) | (1ULL << 10) | 0x3ULL;
        write_memory_64(l1_table_addr + (l1_index * 8), l1_desc_high);

        // L2[0]: VIRTUAL_TEST_ADDR range -> L3 table
        uint32_t l2_index = (VIRTUAL_TEST_ADDR >> 21) & 0x1FF;
        uint64_t l2_desc = (l3_table_addr & ~0xFFFULL) | (1ULL << 10) | 0x3ULL;
        write_memory_64(l2_table_addr + (l2_index * 8), l2_desc);

        // L3[0]: Initialize with CPU-specific default valid mapping to prevent "bad descriptor" faults
        // Each CPU gets its own default region to avoid conflicts
        uint64_t default_physical_addr = REGION_BASE + (cpu * REGION_SIZE); // CPU-specific default region
        uint64_t l3_desc_0 = (default_physical_addr & ~0xFFFULL) | (1ULL << 10) | (3ULL << 8) | (3ULL << 2) |
                             (1ULL << 6) | 0x3ULL;
        write_memory_64(l3_table_addr + (0 * 8), l3_desc_0);

        // CRITICAL DEBUG: Verify the L3[0] descriptor was written to the correct L3 table address
        uint64_t l3_readback = m_memory_accessor.read_memory(l3_table_addr + (0 * 8));

        SCP_INFO(SCMOD) << "  - L3[0]: VA=0x0 -> PA=0x" << std::hex << default_physical_addr << " (CPU " << cpu
                        << " specific default mapping)";
        SCP_INFO(SCMOD) << "  - L3 table address: 0x" << std::hex << l3_table_addr;
        SCP_INFO(SCMOD) << "  - L3[0] descriptor written: 0x" << std::hex << l3_desc_0;
        SCP_INFO(SCMOD) << "  - L3[0] descriptor readback: 0x" << std::hex << l3_readback;
        SCP_INFO(SCMOD) << "  - Writing L3[0] to address: 0x" << std::hex << (l3_table_addr + (0 * 8));

        if (l3_readback != l3_desc_0) {
            SCP_FATAL(SCMOD) << "🚨 CRITICAL: L3[0] descriptor write/read mismatch for CPU " << cpu << "!";
            SCP_FATAL(SCMOD) << "  Expected: 0x" << std::hex << l3_desc_0;
            SCP_FATAL(SCMOD) << "  Got: 0x" << std::hex << l3_readback;
            SCP_FATAL(SCMOD) << "  L3 table address: 0x" << std::hex << l3_table_addr;
            SCP_FATAL(SCMOD) << "  Write address: 0x" << std::hex << (l3_table_addr + (0 * 8));
        } else {
            SCP_INFO(SCMOD) << "✅ L3[0] descriptor verification successful for CPU " << cpu;
        }

        // Configure context bank registers
        uint32_t cb_offset_words = ((16 + cb) * 4096) / 4;
        uint32_t cb_base = SMMU_REG_ADDR + (cb_offset_words * 4);

        // Set CBAR.TYPE = 1 (translation enabled)
        uint32_t cbar_addr = SMMU_REG_ADDR + 0x1000 + (cb * 4);
        write_smmu_register(cbar_addr, (1 << 16)); // TYPE=1

        // Configure TTBR0 with consistent page table address
        write_smmu_register(cb_base + 0x20, static_cast<uint32_t>(page_table_addr & 0xFFFFFFFF));
        write_smmu_register(cb_base + 0x24, static_cast<uint32_t>((page_table_addr >> 32) & 0xFFFFFFFF));

        SCP_INFO(SCMOD) << "  - TTBR0 SET: CB" << cb << " TTBR0=0x" << std::hex << page_table_addr;
        SCP_INFO(SCMOD) << "  - Expected L3 table at: 0x" << std::hex << (page_table_addr + (PAGE_SIZE * 3));

        // Configure TCR for 4KB pages, 48-bit VA space
        write_smmu_register(cb_base + 0x30, (1U << 31) | (16 << 0) | (0 << 14) | (3 << 12) | (1 << 10) | (1 << 8));

        // Configure MAIR for normal memory
        write_smmu_register(cb_base + 0x38, 0xFF);
        write_smmu_register(cb_base + 0x3C, 0x0);

        // Enable MMU with complete page table structure - SINGLE ENABLE OPERATION
        uint32_t sctlr_value = (1 << 0) | (1 << 2) | (1 << 4);
        write_smmu_register(cb_base + 0x0, sctlr_value);

        SCP_INFO(SCMOD) << "✅ CONSOLIDATED SETUP COMPLETE: CPU " << cpu << " CB" << cb;
        SCP_INFO(SCMOD) << "  - Page tables initialized with consistent addressing";
        SCP_INFO(SCMOD) << "  - Context bank configured once with MMU enabled";
        SCP_INFO(SCMOD) << "  - Function conflicts resolved - no duplicate configuration";
        SCP_INFO(SCMOD) << "  - L0[0] -> L1 table, L1[0]: Identity mapping, L1[" << std::dec << l1_index
                        << "] -> High VA";
    }

    void map_cpu_to_region(uint32_t cpu, uint32_t region)
    {
        if (cpu >= m_cpu_to_region.size()) return;

        m_cpu_to_region[cpu] = region;

        // CRITICAL FIX: Use HIGH VA context bank (CB1, CB2) for region mapping
        // Identity context banks (CB0) should remain untouched
        uint32_t high_va_cb = cpu + 1;                             // CB1 for CPU0, CB2 for CPU1
        uint32_t cb_offset_words = ((16 + high_va_cb) * 4096) / 4; // Convert to word offset
        uint32_t cb_base = SMMU_REG_ADDR + (cb_offset_words * 4);  // Convert back to byte address
        uint64_t physical_addr = REGION_BASE + (region * REGION_SIZE);

        // CRITICAL FIX: Use separate page table space for high VA context banks
        // CB1 (CPU 0 high VA) uses offset +4 pages, CB2 (CPU 1 high VA) uses offset +8 pages
        uint64_t page_table_offset = (cpu + 1) * PAGE_SIZE * 4; // CB1=+4 pages, CB2=+8 pages
        uint64_t page_table_addr = PAGE_TABLE_BASE + page_table_offset;

        SCP_INFO(SCMOD) << "🔍 CRITICAL DEBUG: Starting map_cpu_to_region for CPU " << cpu << " -> Region " << region;
        SCP_INFO(SCMOD) << "  - High VA CB: CB" << high_va_cb;
        SCP_INFO(SCMOD) << "  - CB base: 0x" << std::hex << cb_base;
        SCP_INFO(SCMOD) << "  - Page table addr: 0x" << std::hex << page_table_addr;
        SCP_INFO(SCMOD) << "  - Physical addr: 0x" << std::hex << physical_addr;

        // CRITICAL FIX: Create page table mapping BEFORE configuring SMMU registers
        create_page_table_mapping(cpu, VIRTUAL_TEST_ADDR, physical_addr, REGION_SIZE);

        // First disable MMU to safely reconfigure
        SCP_INFO(SCMOD) << "  - Step 1: Disabling MMU for safe reconfiguration";
        write_smmu_register(cb_base + 0x0, 0x0);

        // Add delay after MMU disable
        wait(sc_core::sc_time(1, sc_core::SC_US));

        // Configure TTBR0 with page table address
        SCP_INFO(SCMOD) << "  - Step 2: Configuring TTBR0 with page table address 0x" << std::hex << page_table_addr;
        SCP_INFO(SCMOD) << "    - TTBR0_LOW address: 0x" << std::hex << (cb_base + 0x20);
        SCP_INFO(SCMOD) << "    - TTBR0_HIGH address: 0x" << std::hex << (cb_base + 0x24);
        SCP_INFO(SCMOD) << "    - TTBR0_LOW value: 0x" << std::hex
                        << static_cast<uint32_t>(page_table_addr & 0xFFFFFFFF);
        SCP_INFO(SCMOD) << "    - TTBR0_HIGH value: 0x" << std::hex
                        << static_cast<uint32_t>((page_table_addr >> 32) & 0xFFFFFFFF);

        write_smmu_register(cb_base + 0x20, static_cast<uint32_t>(page_table_addr & 0xFFFFFFFF));
        write_smmu_register(cb_base + 0x24, static_cast<uint32_t>((page_table_addr >> 32) & 0xFFFFFFFF));

        // CRITICAL DEBUG: Read back TTBR0 immediately after writing
        uint32_t ttbr0_low_readback = read_smmu_register(cb_base + 0x20);
        uint32_t ttbr0_high_readback = read_smmu_register(cb_base + 0x24);
        uint64_t ttbr0_readback = (static_cast<uint64_t>(ttbr0_high_readback) << 32) | ttbr0_low_readback;
        SCP_INFO(SCMOD) << "  - Step 2 VERIFICATION: TTBR0 readback immediately after write";
        SCP_INFO(SCMOD) << "    - TTBR0_LOW readback: 0x" << std::hex << ttbr0_low_readback;
        SCP_INFO(SCMOD) << "    - TTBR0_HIGH readback: 0x" << std::hex << ttbr0_high_readback;
        SCP_INFO(SCMOD) << "    - TTBR0 combined: 0x" << std::hex << ttbr0_readback;
        if (ttbr0_readback != page_table_addr) {
            SCP_FATAL(SCMOD) << "🚨 CRITICAL: TTBR0 write/read mismatch! Expected 0x" << std::hex << page_table_addr
                             << ", got 0x" << ttbr0_readback;
        }

        // Configure TCR for 4KB pages, 48-bit VA space
        SCP_INFO(SCMOD) << "  - Step 3: Configuring TCR for 4KB pages";
        uint32_t tcr_value = (1U << 31) | (16 << 0) | (0 << 14) | (3 << 12) | (1 << 10) | (1 << 8);
        write_smmu_register(cb_base + 0x30, tcr_value);

        // Configure MAIR for normal memory
        SCP_INFO(SCMOD) << "  - Step 4: Configuring MAIR";
        write_smmu_register(cb_base + 0x38, 0xFF); // Normal memory, write-back cacheable
        write_smmu_register(cb_base + 0x3C, 0x0);

        // CRITICAL DEBUG: Read back TTBR0 before enabling MMU
        uint32_t ttbr0_low_pre_mmu = read_smmu_register(cb_base + 0x20);
        uint32_t ttbr0_high_pre_mmu = read_smmu_register(cb_base + 0x24);
        uint64_t ttbr0_pre_mmu = (static_cast<uint64_t>(ttbr0_high_pre_mmu) << 32) | ttbr0_low_pre_mmu;
        SCP_INFO(SCMOD) << "  - Step 4 VERIFICATION: TTBR0 readback before MMU enable";
        SCP_INFO(SCMOD) << "    - TTBR0 before MMU: 0x" << std::hex << ttbr0_pre_mmu;
        if (ttbr0_pre_mmu != page_table_addr) {
            SCP_FATAL(SCMOD) << "🚨 CRITICAL: TTBR0 corrupted before MMU enable! Expected 0x" << std::hex
                             << page_table_addr << ", got 0x" << ttbr0_pre_mmu;
        }

        // Add delay before enabling MMU
        wait(sc_core::sc_time(2, sc_core::SC_US));

        // Enable MMU with proper configuration - CRITICAL: M bit must be set for translation
        uint32_t sctlr_value = (1 << 0) | (1 << 2) | (1 << 4);
        SCP_INFO(SCMOD) << "  - Step 5: Enabling MMU with SCTLR: 0x" << std::hex << sctlr_value;
        write_smmu_register(cb_base + 0x0, sctlr_value);

        // Additional delay to ensure MMU enable takes effect
        wait(sc_core::sc_time(3, sc_core::SC_US));

        // CRITICAL DEBUG: Read back all critical registers after MMU enable
        uint32_t sctlr_readback = read_smmu_register(cb_base + 0x0);
        uint32_t ttbr0_low_final = read_smmu_register(cb_base + 0x20);
        uint32_t ttbr0_high_final = read_smmu_register(cb_base + 0x24);
        uint64_t ttbr0_final = (static_cast<uint64_t>(ttbr0_high_final) << 32) | ttbr0_low_final;

        bool mmu_enabled = (sctlr_readback & 0x1) != 0;

        SCP_INFO(SCMOD) << "  - Step 5 FINAL VERIFICATION for CB" << high_va_cb << ":";
        SCP_INFO(SCMOD) << "    - SCTLR write: 0x" << std::hex << sctlr_value;
        SCP_INFO(SCMOD) << "    - SCTLR read: 0x" << std::hex << sctlr_readback;
        SCP_INFO(SCMOD) << "    - TTBR0 expected: 0x" << std::hex << page_table_addr;
        SCP_INFO(SCMOD) << "    - TTBR0 final: 0x" << std::hex << ttbr0_final;
        SCP_INFO(SCMOD) << "    - MMU enabled: " << (mmu_enabled ? "YES" : "NO");

        if (!mmu_enabled) {
            SCP_FATAL(SCMOD) << "🚨 CRITICAL: MMU not enabled for high VA CB" << high_va_cb
                             << " - SCTLR.M bit not set!";
        }

        if (ttbr0_final != page_table_addr) {
            SCP_FATAL(SCMOD) << "🚨 CRITICAL: TTBR0 corrupted after MMU enable! Expected 0x" << std::hex
                             << page_table_addr << ", got 0x" << ttbr0_final;
        }

        SCP_INFO(SCMOD) << "✅ SMMU mapping completed successfully for CPU " << cpu << " -> Region " << region;
        SCP_INFO(SCMOD) << "   Final state: CB" << high_va_cb << " TTBR0=0x" << std::hex << ttbr0_final << " SCTLR=0x"
                        << sctlr_readback << " MMU=" << (mmu_enabled ? "ON" : "OFF");

        // DIAGNOSTIC: show SCTLR live just prior to region assignment
        uint32_t sctlr_live = read_smmu_register(cb_base + 0x0);
        SCP_INFO(SCMOD) << "DIAGNOSTIC: CB" << high_va_cb << " SCTLR immediately before region READY: 0x" << std::hex
                        << sctlr_live << " (MMU enabled? " << ((sctlr_live & 0x1) ? "YES" : "NO") << ")";
    }

    void create_page_table_mapping(uint32_t cpu, uint64_t virtual_addr, uint64_t physical_addr, uint64_t size)
    {
        // CRITICAL FIX: Create separate page tables for HIGH VA context banks (CB2/CB3)
        // CB2/CB3 need L1[0] to map VA=0x0 directly to the region's physical address
        // This is different from CB0/CB1 which use identity mapping

        // Use separate page table space for high VA context banks
        // CB1 (CPU 0 high VA) uses offset +4 pages, CB2 (CPU 1 high VA) uses offset +8 pages
        uint32_t high_va_cpu = cpu;                                     // This function is called for high VA mapping
        uint64_t page_table_offset = (high_va_cpu + 1) * PAGE_SIZE * 4; // CB1=+4 pages, CB2=+8 pages

        uint64_t l0_table_addr = PAGE_TABLE_BASE + page_table_offset;                   // L0 table for high VA CB
        uint64_t l1_table_addr = PAGE_TABLE_BASE + page_table_offset + PAGE_SIZE;       // L1 table for high VA CB
        uint64_t l2_table_addr = PAGE_TABLE_BASE + page_table_offset + (PAGE_SIZE * 2); // L2 table for high VA CB
        uint64_t l3_table_addr = PAGE_TABLE_BASE + page_table_offset + (PAGE_SIZE * 3); // L3 table for high VA CB

        SCP_INFO(SCMOD) << "🔍 CRITICAL FIX: Creating HIGH VA page tables for CPU " << cpu;
        SCP_INFO(SCMOD) << "  - High VA CB page table offset: 0x" << std::hex << page_table_offset;
        SCP_INFO(SCMOD) << "  - L0 table: 0x" << std::hex << l0_table_addr;
        SCP_INFO(SCMOD) << "  - L1 table: 0x" << std::hex << l1_table_addr;
        SCP_INFO(SCMOD) << "  - L2 table: 0x" << std::hex << l2_table_addr;
        SCP_INFO(SCMOD) << "  - L3 table: 0x" << std::hex << l3_table_addr;

        // OPTIMIZED: Only update L3[0] entry - page table structure already exists from setup
        // The complete L0→L1→L2→L3 structure was created in setup_complete_high_va_context_bank()
        // We only need to update the final L3[0] mapping to point to the new region

        // Update L3[0] to map VA=0x0 to the NEW region's physical address
        // SH=0b11 (inner shareable) at bits [3:2], AF=1, AttrIndx=0b011, NS=1, page type=0b11
        uint64_t l3_desc_0 = (physical_addr & ~0xFFFULL) | (1ULL << 10) | (3ULL << 8) | (3ULL << 2) | (1ULL << 6) |
                             0x3ULL;
        write_memory_64(l3_table_addr + (0 * 8), l3_desc_0);

        // Verify the L3[0] update was successful
        uint64_t l3_readback = m_memory_accessor.read_memory(l3_table_addr + (0 * 8));
        uint64_t extracted_physical = l3_readback & ~0xFFFULL;

        SCP_INFO(SCMOD) << "✅ OPTIMIZED page table update for CPU " << cpu << ":";
        SCP_INFO(SCMOD) << "  - L3[0]: VA=0x0 -> PA=0x" << std::hex << physical_addr << " (region "
                        << ((physical_addr - REGION_BASE) / REGION_SIZE) << ")";
        SCP_INFO(SCMOD) << "  - L3[0] descriptor: 0x" << std::hex << l3_desc_0;
        SCP_INFO(SCMOD) << "  - L3[0] readback: 0x" << std::hex << l3_readback;
        SCP_INFO(SCMOD) << "  - Extracted physical: 0x" << std::hex << extracted_physical;

        if (extracted_physical != physical_addr) {
            SCP_FATAL(SCMOD) << "🚨 CRITICAL: L3[0] physical address mismatch!";
            SCP_FATAL(SCMOD) << "  Expected: 0x" << std::hex << physical_addr;
            SCP_FATAL(SCMOD) << "  Found: 0x" << std::hex << extracted_physical;
        } else {
            SCP_INFO(SCMOD) << "✅ L3[0] physical address verification successful";
        }

        // CRITICAL FIX: Invalidate SMMU TLB after page table update
        // The SMMU TLB caches old translations and must be invalidated when page tables change
        invalidate_smmu_tlb(cpu);
    }

    void unmap_cpu_region(uint32_t cpu)
    {
        if (cpu >= m_cpu_to_region.size()) return;

        // CRITICAL FIX: Only unmap HIGH VA context banks (CB1, CB2, CB3...)
        // NEVER touch CB0 (shared identity context bank used by ALL CPUs)
        uint32_t high_va_cb = cpu + 1; // CB1 for CPU0, CB2 for CPU1, CB3 for CPU2...

        // SAFETY CHECK: Ensure we never accidentally target CB0 (shared by all CPUs)
        if (high_va_cb == 0) {
            SCP_FATAL(SCMOD) << "🚨 CRITICAL BUG: Attempted to unmap shared CB0! This would affect ALL CPUs!";
            SCP_FATAL(SCMOD) << "  CPU " << cpu << " should map to CB" << (cpu + 1) << ", not CB0";
            return;
        }

        uint32_t cb_offset_words = ((16 + high_va_cb) * 4096) / 4; // Convert to word offset
        uint32_t cb_base = SMMU_REG_ADDR + (cb_offset_words * 4);  // Convert back to byte address

        SCP_INFO(SCMOD) << "🔒 CROSS-CPU SAFE UNMAP: CPU " << cpu << " unmapping CB" << high_va_cb
                        << " (CB0 shared identity remains untouched)";

        // Only disable the CPU-specific high VA context bank MMU
        // CB0 (shared identity) remains active for all CPUs
        write_smmu_register(cb_base + 0x0, 0x0); // Disable MMU in high VA CB only

        // Clear TTBR registers for this CPU's high VA context bank only
        write_smmu_register(cb_base + 0x20, 0x0);
        write_smmu_register(cb_base + 0x24, 0x0);

        SCP_INFO(SCMOD) << "✅ SAFE UNMAP COMPLETE: CPU " << cpu << " high VA CB" << high_va_cb
                        << " disabled, CB0 identity mapping preserved for all CPUs";
        m_cpu_to_region[cpu] = 0xFFFFFFFF;
    }

    void generate_tester_controlled_firmware()
    {
        SCP_INFO(SCMOD) << "Generating tester-controlled firmware with proper layout";

        // 1. BOOT LOADER at 0x0 - Just jumps to main firmware
        static constexpr const char* BOOT_LOADER = R"(
            // BOOT LOADER at 0x0
            boot_start:
                // Jump to main firmware at 0x1000
                movz x0, #0x%04)" PRIx32 R"(  // Main firmware address (0x1000)
                br x0                         // Jump to main firmware
        )";

        char boot_buf[256];
        std::snprintf(boot_buf, sizeof(boot_buf), BOOT_LOADER, static_cast<uint32_t>(MAIN_FIRMWARE_ADDR));
        set_firmware(boot_buf, BOOT_ADDR);

        // 2. DIAGNOSTIC HANDLER at 0x200 - Error reporting
        static constexpr const char* DIAGNOSTIC_HANDLER = R"(
            // DIAGNOSTIC ERROR HANDLER at 0x200
            diagnostic_error:
                // Get CPU ID
                mrs x0, mpidr_el1
                and x0, x0, #0xff
                
                // Calculate tester base address for this CPU
                ldr x1, =0x%08)" PRIx64 R"(    // TESTER_ADDR
                mov x2, #0x100                 // Register size per CPU
                mul x2, x0, x2                 // CPU offset
                add x1, x1, x2                // x1 = CPU-specific tester base
                
                // Send error message: 0xE000 + CPU_ID
                mov x2, #0xE000
                orr x2, x2, x0                // Error code + CPU ID
                str x2, [x1, #0x28]           // Write to REG_DEBUG
                
                // Stop the test with error
                mov x0, #1                    // Exit with error
                hlt #0                        // Halt with error
                
            diagnostic_loop:
                wfi
                b diagnostic_loop
        )";

        char diagnostic_buf[1024];
        std::snprintf(diagnostic_buf, sizeof(diagnostic_buf), DIAGNOSTIC_HANDLER, TESTER_ADDR);
        set_firmware(diagnostic_buf, DIAGNOSTIC_ADDR);

        // 3. MAIN FIRMWARE at 0x1000 - The actual test logic
        static constexpr const char* MAIN_FIRMWARE = R"(
            // MAIN FIRMWARE at 0x1000
            main_start:
                // Get CPU ID
                mrs x0, mpidr_el1
                and x0, x0, #0xff
                mov x20, x0                    // Save CPU ID in x20

                // Calculate tester base address for this CPU
                ldr x1, =0x%08)" PRIx64 R"(    // TESTER_ADDR
                mov x2, #0x100                 // Register size per CPU
                mul x2, x20, x2                // CPU offset
                add x21, x1, x2               // x21 = CPU-specific tester base

                // Signal startup
                mov x0, #0x1000
                str x0, [x21, #0x28]          // Write to REG_DEBUG

                // Initialize iteration counter
                mov x22, #0                   // x22 = iteration counter

            main_loop:
                // Check if we've reached max iterations
                mov x3, #%d                   // Max iterations
                cmp x22, x3
                b.ge test_complete

                // Signal entering main loop
                mov x0, #0x2000
                orr x0, x0, x22               // Include iteration count
                str x0, [x21, #0x28]          // Write to REG_DEBUG

                // Try to get a region to fill
                mov x0, #1                    // FILL_REQUEST
                str x0, [x21, #0x00]          // Write to REG_REQUEST

                // Poll for readiness
            poll_fill:
                mov x0, #0x3000               // Polling debug message
                str x0, [x21, #0x28]          // Write to REG_DEBUG
                
                ldr x0, [x21, #0x08]          // Read REG_STATUS
                cmp x0, #1                    // READY?
                b.eq fill_ready
                cmp x0, #0                    // BUSY?
                b.eq try_check                // Try checking instead
                b poll_fill

            fill_ready:
                // Get assigned region ID
                ldr x23, [x21, #0x10]         // Read REG_REGION_ID
                
                // Signal starting work
                mov x0, #0x4000
                orr x0, x0, x23               // Include region ID
                str x0, [x21, #0x28]          // Write to REG_DEBUG

                // Fill the region at virtual address
                ldr x3, =0x%016)" PRIx64 R"(   // VIRTUAL_TEST_ADDR
                mov x4, #%llu                 // Pattern size
                mov x5, #0                    // Offset counter
                
                // DEBUG: Signal start of fill with first address
                mov x0, #0x6000
                orr x0, x0, x23               // Include region ID
                str x0, [x21, #0x28]          // Write to REG_DEBUG
                
            fill_loop:
                cmp x5, x4
                b.ge fill_done
                
                // Generate pattern fresh each iteration to avoid corruption
                lsl x24, x20, #32             // CPU_ID << 32
                lsl x25, x23, #16             // region_ID << 16
                orr x24, x24, x25
                orr x24, x24, x22             // Add iteration counter
                
                lsl x6, x5, #3                // offset * 8
                add x6, x3, x6                // Virtual address + offset
                
                // DEBUG: Log first few writes with address and pattern
                cmp x5, #4
                b.ge skip_debug_write
                mov x0, #0x7000
                orr x0, x0, x5                // Include offset
                str x0, [x21, #0x28]          // Write to REG_DEBUG
                str x24, [x21, #0x30]
                str x6, [x21, #0x30]                

            skip_debug_write:
                str x24, [x6]                 // Store pattern
                
                add x5, x5, #1
                b fill_loop
            fill_done:
                
                // DEBUG: Signal end of fill
                mov x0, #0x8000
                orr x0, x0, x23               // Include region ID
                str x0, [x21, #0x28]          // Write to REG_DEBUG

                // Signal fill complete
                mov x0, #1                    // FILL_DONE
                str x0, [x21, #0x18]          // Write to REG_COMPLETE

                // Increment iteration
                add x22, x22, #1
                b main_loop

            try_check:
                // Try to get a region to check
                mov x0, #2                    // CHECK_REQUEST
                str x0, [x21, #0x00]          // Write to REG_REQUEST

                // Poll for readiness
            poll_check:
                ldr x0, [x21, #0x08]          // Read REG_STATUS
                cmp x0, #1                    // READY?
                b.eq check_ready
                cmp x0, #0                    // BUSY?
                b.eq main_loop                // Back to main loop
                b poll_check

            check_ready:
                // Get assigned region ID
                ldr x23, [x21, #0x10]         // Read REG_REGION_ID
                
                // Signal starting work
                mov x0, #0x4000
                orr x0, x0, x23               // Include region ID
                str x0, [x21, #0x28]          // Write to REG_DEBUG

                // Verify region pattern (check first few words)
                ldr x3, =0x%016)" PRIx64 R"(   // VIRTUAL_TEST_ADDR
                mov x5, #0                    // Offset counter
                mov x6, #4                    // Check first 4 words
            verify_loop:
                cmp x5, x6
                b.ge verify_success
                
                lsl x7, x5, #3                // offset * 8
                add x7, x3, x7                // Virtual address + offset
                ldr x8, [x7]                  // Load value
                
                // Extract region ID from pattern (bits 16-31)
                lsr x9, x8, #16
                and x9, x9, #0xFFFF
                cmp x9, x23
                b.ne pattern_error
                
                add x5, x5, #1
                b verify_loop

            verify_success:
                // Signal verify success
                mov x0, #0x5000
                str x0, [x21, #0x28]          // Write to REG_DEBUG

                // Clear region
                ldr x3, =0x%016)" PRIx64 R"(   // VIRTUAL_TEST_ADDR
                mov x4, #%llu                 // Pattern size
                mov x7, #0                    // Zero value
                mov x5, #0                    // Offset counter
            clear_loop:
                cmp x5, x4
                b.ge clear_done
                
                lsl x8, x5, #3                // offset * 8
                add x8, x3, x8                // Virtual address + offset
                str x7, [x8]                  // Store zero
                
                add x5, x5, #1
                b clear_loop
            clear_done:

                // Signal check complete
                mov x0, #2                    // CHECK_DONE
                str x0, [x21, #0x18]          // Write to REG_COMPLETE

                // Increment iteration
                add x22, x22, #1
                b main_loop

            pattern_error:
                // Signal pattern verification failure
                mov x0, #0xD000
                str x0, [x21, #0x28]          // Write to REG_DEBUG
                b end

            test_complete:
                // Signal completion - tester will handle global coordination
//                mov x0, #0x1000
//                add x0, x0, x20               // Success marker + CPU ID
//                str x0, [x21, #0x28]          // Write to REG_DEBUG
                b end

            end:
                wfi
                b end
        )";

        char main_buf[8192];
        std::snprintf(main_buf, sizeof(main_buf), MAIN_FIRMWARE,
                      TESTER_ADDR,       // Parameter 1: TESTER_ADDR
                      MAX_ITERATIONS,    // Parameter 2: MAX_ITERATIONS
                      VIRTUAL_TEST_ADDR, // Parameter 3: VIRTUAL_TEST_ADDR (fill)
                      PATTERN_SIZE,      // Parameter 4: PATTERN_SIZE (fill)
                      VIRTUAL_TEST_ADDR, // Parameter 5: VIRTUAL_TEST_ADDR (verify)
                      VIRTUAL_TEST_ADDR, // Parameter 6: VIRTUAL_TEST_ADDR (clear)
                      PATTERN_SIZE);     // Parameter 7: PATTERN_SIZE (clear)

        set_firmware(main_buf, MAIN_FIRMWARE_ADDR);

        SCP_INFO(SCMOD) << "Firmware layout completed:";
        SCP_INFO(SCMOD) << "  - Boot loader at 0x" << std::hex << BOOT_ADDR;
        SCP_INFO(SCMOD) << "  - Diagnostic handler at 0x" << std::hex << DIAGNOSTIC_ADDR;
        SCP_INFO(SCMOD) << "  - Main firmware at 0x" << std::hex << MAIN_FIRMWARE_ADDR;
    }

    void timeout_monitor()
    {
        SCP_INFO(SCMOD) << "Starting timeout monitor - test will terminate after " << TEST_DURATION;

        wait(TEST_DURATION);

        SCP_WARN(SCMOD) << "⏰ TIMEOUT: Test terminated after " << TEST_DURATION;
        SCP_INFO(SCMOD) << "Final iterations: " << m_tester_controller.m_global_iterations << "/" << MAX_ITERATIONS;

        if (m_tester_controller.m_global_iterations > 0) {
            SCP_INFO(SCMOD) << "✓ SMMU stress test V2 COMPLETED with timeout";
        } else {
            SCP_WARN(SCMOD) << "⚠ Test completed with no iterations";
        }

        sc_core::sc_stop();
    }

    void write_smmu_register(uint32_t addr, uint32_t value)
    {
        SCP_INFO(SCMOD) << "ATTEMPTING SMMU WRITE: addr=0x" << std::hex << addr << ", value=0x" << value;
        m_memory_accessor.write_register(addr, value);
        SCP_INFO(SCMOD) << "SMMU WRITE COMPLETED: addr=0x" << std::hex << addr << ", value=0x" << value;
    }

    uint32_t read_smmu_register(uint32_t addr)
    {
        SCP_INFO(SCMOD) << "ATTEMPTING SMMU READ: addr=0x" << std::hex << addr;
        uint32_t value = m_memory_accessor.read_register(addr);
        SCP_INFO(SCMOD) << "SMMU READ COMPLETED: addr=0x" << std::hex << addr << ", value=0x" << value;
        return value;
    }

    void write_memory_64(uint64_t addr, uint64_t value) { m_memory_accessor.write_memory(addr, value); }

    void invalidate_smmu_tlb(uint32_t cpu)
    {
        // CRITICAL FIX: Invalidate SMMU TLB after page table updates
        // The SMMU TLB caches old translations and must be invalidated when page tables change

        uint32_t high_va_cb = cpu + 1; // CB1 for CPU0, CB2 for CPU1
        uint32_t cb_offset_words = ((16 + high_va_cb) * 4096) / 4;
        uint32_t cb_base = SMMU_REG_ADDR + (cb_offset_words * 4);

        SCP_INFO(SCMOD) << "🔄 TLB INVALIDATION: Invalidating SMMU TLB for CPU " << cpu << " (CB" << high_va_cb
                        << ") - interrupts disabled during setup";

        // Method 1: Write to TLBIALL register to invalidate all TLB entries for this context bank
        // TLBIALL is per-CB and only affects this specific context bank
        // Since we disabled CFIE during setup, no completion interrupts will be generated
        uint32_t tlbiall_addr = cb_base + 0x618;
        write_smmu_register(tlbiall_addr, 0x0); // Any write invalidates all entries for this CB

        // Method 2: Also invalidate by virtual address (TLBIVA) for the specific VA range
        // TLBIVA is per-CB and targets specific virtual addresses
        //        uint32_t tlbiva_addr = cb_base + 0x600;
        //        write_smmu_register(tlbiva_addr, 0x0); // Invalidate VA=0x0 (our mapped address)

        // Add delay to ensure TLB invalidation takes effect
        wait(sc_core::sc_time(2, sc_core::SC_US));

        SCP_INFO(SCMOD) << "✅ TLB INVALIDATION COMPLETE: CPU " << cpu << " TLB cleared";
    }

    // CpuTesterCallbackIface implementation (not used in V2)
    virtual void map_target(tlm::tlm_target_socket<DEFAULT_TLM_BUSWIDTH>& s, uint64_t addr, uint64_t size) override {}
    virtual void map_irqs_to_cpus(sc_core::sc_vector<InitiatorSignalSocket<bool> >& irqs) override {}
    virtual uint64_t mmio_read(int id, uint64_t addr, size_t len) override { return 0; }
    virtual bool dmi_request(int id, uint64_t addr, size_t len, tlm::tlm_dmi& ret) override { return false; }
    virtual void mmio_write(int id, uint64_t addr, uint64_t data, size_t len) override {}

    virtual void end_of_simulation() override
    {
        SCP_INFO(SCMOD) << "SMMU Stress Test V2 completed";
        SCP_INFO(SCMOD) << "Final statistics:";
        SCP_INFO(SCMOD) << "  - Total iterations: " << m_tester_controller.m_global_iterations << "/" << MAX_ITERATIONS;
        SCP_INFO(SCMOD) << "  - CPUs: " << p_num_cpu.get_value();
        SCP_INFO(SCMOD) << "  - Memory regions: " << m_num_regions;
        SCP_INFO(SCMOD) << "  - Pattern size: " << PATTERN_SIZE << " * 8 bytes";
    }
};

// Implement the tester controller methods that need access to parent
void SMMUTesterController::configure_smmu_mapping(uint32_t cpu_id, uint32_t region_id)
{
    if (m_parent) {
        m_parent->map_cpu_to_region(cpu_id, region_id);
    }
}

void SMMUTesterController::unmap_cpu_region(uint32_t cpu_id)
{
    if (m_parent) {
        m_parent->unmap_cpu_region(cpu_id);
    }
}

// NEW METHODS: Tester directly verifies region patterns in physical memory
bool SMMUTesterController::verify_region_pattern(uint32_t cpu_id, uint32_t region_id)
{
    if (!m_parent) {
        SCP_FATAL(SCMOD) << "🚨 CRITICAL: No parent reference for memory access";
        return false;
    }

    // Calculate the physical address of the region
    uint64_t physical_addr = CpuArmCortexA53SMMUStressTestV2::REGION_BASE +
                             (region_id * CpuArmCortexA53SMMUStressTestV2::REGION_SIZE);

    SCP_INFO(SCMOD) << "🔍 TESTER VERIFICATION: Checking region " << region_id << " at physical address 0x" << std::hex
                    << physical_addr;

    // Read the first few 64-bit words from the physical region
    const uint32_t words_to_check = 4;
    bool verification_success = true;

    for (uint32_t word_offset = 0; word_offset < words_to_check; ++word_offset) {
        uint64_t word_addr = physical_addr + (word_offset * 8);
        uint64_t read_value = m_parent->m_memory_accessor.read_memory(word_addr);

        // Extract components from the pattern
        // Pattern format: (cpu_id << 32) | (region_id << 16) | iteration
        uint32_t pattern_cpu_id = (read_value >> 32) & 0xFFFFFFFF;
        uint32_t pattern_region_id = (read_value >> 16) & 0xFFFF;
        uint32_t pattern_iteration = read_value & 0xFFFF;

        SCP_INFO(SCMOD) << "  Word[" << word_offset << "] at 0x" << std::hex << word_addr << ": value=0x" << read_value
                        << " (CPU=" << std::dec << pattern_cpu_id << ", Region=" << pattern_region_id
                        << ", Iter=" << pattern_iteration << ")";

        SCP_INFO(SCMOD) << "  Expected: CPU=" << cpu_id << ", Region=" << region_id;

        // Verify that the region ID matches
        if (pattern_region_id != region_id) {
            SCP_FATAL(SCMOD) << "🚨 PATTERN MISMATCH: Expected region " << region_id << ", found " << pattern_region_id
                             << " in word " << word_offset;
            SCP_FATAL(SCMOD) << "  This indicates a bug in region assignment or CPU pattern generation";
            verification_success = false;
            break;
        }

        // CORRECTED FIX: Verify CPU ID matches expected CPU (the original filler)
        if (pattern_cpu_id != cpu_id) {
            SCP_FATAL(SCMOD) << "🚨 CPU ID MISMATCH: Expected CPU " << cpu_id << ", found " << pattern_cpu_id
                             << " in word " << word_offset;
            SCP_FATAL(SCMOD) << "  This indicates the wrong CPU filled this region or pattern corruption";
            verification_success = false;
            break;
        }

        // Check that the pattern is not zero (indicating it was actually written)
        if (read_value == 0) {
            //            SCP_FATAL(SCMOD) << "🚨 PATTERN MISSING: Found zero value in word " << word_offset
            //                             << " - pattern was not written or was cleared";
            //            verification_success = false;
            //            break;
        }
    }

    if (verification_success) {
        SCP_INFO(SCMOD) << "✅ TESTER VERIFICATION SUCCESS: Region " << region_id << " contains valid pattern from CPU "
                        << cpu_id;
    } else {
        SCP_FATAL(SCMOD) << "❌ TESTER VERIFICATION FAILED: Region " << region_id << " pattern verification failed";
    }

    return verification_success;
}

void SMMUTesterController::clear_region_pattern(uint32_t region_id)
{
    if (!m_parent) {
        SCP_FATAL(SCMOD) << "🚨 CRITICAL: No parent reference for memory access";
        return;
    }

    // Calculate the physical address of the region
    uint64_t physical_addr = CpuArmCortexA53SMMUStressTestV2::REGION_BASE +
                             (region_id * CpuArmCortexA53SMMUStressTestV2::REGION_SIZE);

    SCP_INFO(SCMOD) << "🧹 TESTER CLEANUP: Clearing region " << region_id << " at physical address 0x" << std::hex
                    << physical_addr;

    // Clear the entire region by writing zeros
    uint64_t pattern_size_words = CpuArmCortexA53SMMUStressTestV2::PATTERN_SIZE;

    for (uint64_t word_offset = 0; word_offset < pattern_size_words; ++word_offset) {
        uint64_t word_addr = physical_addr + (word_offset * 8);
        m_parent->write_memory_64(word_addr, 0);
    }

    SCP_INFO(SCMOD) << "✅ TESTER CLEANUP COMPLETE: Region " << region_id << " cleared (" << std::dec
                    << pattern_size_words << " words zeroed)";
}

/* ---- Implementation moved here to ensure CpuArmCortexA53SMMUStressTestV2 is a complete type ---- */

void SMMUTesterController::handle_complete(uint32_t cpu_id, CompleteType complete)
{
    CPUState& cpu = m_cpu_states[cpu_id];

    if (complete == FILL_DONE) {
        SCP_INFO(SCMOD) << "🔍 CRITICAL DEBUG: handle_complete called for CPU " << cpu_id;
        SCP_INFO(SCMOD) << "  - CPU state assigned_region: " << cpu.assigned_region;
        SCP_INFO(SCMOD) << "  - CPU state current_request: " << (int)cpu.current_request;
        SCP_INFO(SCMOD) << "  - CPU state is_working: " << cpu.is_working;

        // CRITICAL: Validate that assigned_region is reasonable
        if (cpu.assigned_region >= m_num_regions) {
            SCP_FATAL(SCMOD) << "🚨 CRITICAL BUG: CPU " << cpu_id << " has invalid assigned_region "
                             << cpu.assigned_region << " (max regions: " << m_num_regions << ")";
            sc_core::sc_stop();
            return;
        }

        SCP_INFO(SCMOD) << "CPU " << cpu_id << " completed filling region " << cpu.assigned_region;

        // Diagnostic: dump first 4 words BEFORE verification
        if (m_parent) {
            uint64_t phys = CpuArmCortexA53SMMUStressTestV2::REGION_BASE +
                            (cpu.assigned_region * CpuArmCortexA53SMMUStressTestV2::REGION_SIZE);
            SCP_INFO(SCMOD) << "DEBUG: Region " << cpu.assigned_region << " @0x" << std::hex << phys
                            << " BEFORE verify_region_pattern:";
            for (uint64_t i = 0; i < 4; ++i) {
                uint64_t val = m_parent->m_memory_accessor.read_memory(phys + 8 * i);
                SCP_INFO(SCMOD) << "  Pre-verif word[" << i << "] = 0x" << std::hex << val;
            }
        }

        // NEW APPROACH: Extract the original filler CPU from the pattern and verify against that
        // First read the pattern to determine which CPU originally filled this region
        uint64_t physical_addr = CpuArmCortexA53SMMUStressTestV2::REGION_BASE +
                                 (cpu.assigned_region * CpuArmCortexA53SMMUStressTestV2::REGION_SIZE);
        uint64_t first_word = m_parent->m_memory_accessor.read_memory(physical_addr);
        uint32_t original_filler_cpu = (first_word >> 32) & 0xFFFFFFFF;

        SCP_INFO(SCMOD) << "🔍 PATTERN ANALYSIS: Region " << cpu.assigned_region << " first word=0x" << std::hex
                        << first_word << " indicates original filler CPU=" << std::dec << original_filler_cpu;

        // Verify against the original filler CPU, not the current CPU
        bool verification_success = verify_region_pattern(original_filler_cpu, cpu.assigned_region);

        // Diagnostic: dump after verify (regardless of result)
        if (m_parent) {
            uint64_t phys = CpuArmCortexA53SMMUStressTestV2::REGION_BASE +
                            (cpu.assigned_region * CpuArmCortexA53SMMUStressTestV2::REGION_SIZE);
            SCP_INFO(SCMOD) << "DEBUG: Region " << cpu.assigned_region << " @0x" << std::hex << phys
                            << " AFTER verify_region_pattern:";
            for (uint64_t i = 0; i < 4; ++i) {
                uint64_t val = m_parent->m_memory_accessor.read_memory(phys + 8 * i);
                SCP_INFO(SCMOD) << "  Post-verif word[" << i << "] = 0x" << std::hex << val;
            }
        }

        if (verification_success) {
            SCP_INFO(SCMOD) << "✅ TESTER VERIFICATION SUCCESS: Region " << cpu.assigned_region
                            << " pattern verified by tester controller";

            // Clear the region after successful verification
            clear_region_pattern(cpu.assigned_region);

            // Return region to available queue immediately
            m_available_regions.push(cpu.assigned_region);

            // Increment iteration count for successful fill+verify cycle
            cpu.iteration_count++;
            m_global_iterations++;

        } else {
            SCP_FATAL(SCMOD) << "🚨 TESTER VERIFICATION FAILED: Region " << cpu.assigned_region
                             << " pattern verification failed - stopping test";
            sc_core::sc_stop();
            return;
        }

    } else if (complete == CHECK_DONE) {
        // This branch should no longer be used with the new approach
        SCP_WARN(SCMOD) << "CPU " << cpu_id << " reported CHECK_DONE - this should not happen with new approach";

        // Return region to available queue
        m_available_regions.push(cpu.assigned_region);

        // Increment iteration count
        cpu.iteration_count++;
        m_global_iterations++;
    }

    // Unmap region from CPU
    unmap_cpu_region(cpu_id);

    // Reset CPU state
    cpu.current_request = NONE;
    cpu.assigned_region = 0xFFFFFFFF;
    cpu.is_working = false;
    cpu.status = COMPLETE;

    // Check if we've reached target iterations
    if (m_global_iterations >= m_target_iterations) {
        SCP_INFO(SCMOD) << "✅ TARGET REACHED: " << m_global_iterations << "/" << m_target_iterations
                        << " iterations completed";
        //        sc_core::sc_stop();
    }
}

int sc_main(int argc, char* argv[])
{
    scp::init_logging(scp::LogConfig()
                          .fileInfoFrom(sc_core::SC_ERROR)
                          .logAsync(false)
                          .logLevel(scp::log::INFO)
                          .msgTypeFieldWidth(30));

    gs::ConfigurableBroker m_broker{};
    cci::cci_originator orig{ "sc_main" };
    auto broker_h = m_broker.create_broker_handle(orig);
    ArgParser ap{ broker_h, argc, argv };

    SCP_INFO("sc_main") << "Start SMMU Stress Test V2 - Tester Controlled";
    CpuArmCortexA53SMMUStressTestV2 test_bench("test-bench");

    if ((gs::cci_get<int>(broker_h,"test-bench.num_cpu") > 8) ||
        gs::cci_get_d<bool>(broker_h, "test-bench.inst_a.icount", false)) {
        SCP_INFO("sc_main")("Refusing to run more than 8 CPUs, or icount mode");
        exit(0);
    }
    try {
        sc_core::sc_start();
    } catch (std::exception& e) {
        std::cerr << "Test failure: " << e.what() << "\n";
        SCP_INFO("sc_main") << "Test failed";
        return 1;
    } catch (...) {
        std::cerr << "Test failure: Unknown exception\n";
        SCP_INFO("sc_main") << "Test failed";
        return 1;
    }

    SCP_INFO("sc_main") << "Test done";
    exit(0);
    return 0;
}
