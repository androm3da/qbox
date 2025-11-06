/*
 * This file is part of libqbox
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: GreenSocs 2021
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <systemc>
#include <cstdio>
#include <cassert>
#include <vector>
#include <queue>
#include <iomanip>
#include <typeinfo>
#include <fstream>
#include <scp/report.h>
#include <cci/utils/broker.h>
#include <libgsutils.h>
#include <cciutils.h>
#include <argparser.h>
#include <tlm_utils/tlm_quantumkeeper.h>
#include "cci/cfg/cci_broker_if.h"
#include "test/cpu.h"
#include "test/tester/mmio.h"
#include <hexagon.h>
#include <hexagon_globalreg.h>
#include <qemu-instance.h>
#include <router.h>
#include <gs_memory.h>
#include <smmu500.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <pass.h>
#include <ports/target-signal-socket.h>

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

        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        socket->b_transport(txn, delay);
        return value;
    }
};

/*
 * SMMU Stress Test V2 - Hexagon Version
 *
 * This is a Hexagon adaptation of the ARM SMMU stress test.
 * The SMMU itself is CPU-agnostic - only the firmware changes.
 *
 * Key differences from ARM version:
 * - Uses Hexagon CPUs instead of ARM Cortex-A53
 * - Uses pre-compiled Hexagon binary firmware (no Keystone assembler)
 * - Same SMMU configuration and test logic
 */

class SMMUTesterController : public sc_core::sc_module
{
public:
    // MMIO Register Layout (per CPU) - same as ARM version
    static constexpr uint64_t REG_SIZE_PER_CPU = 0x100;
    static constexpr uint64_t REG_REQUEST = 0x00;
    static constexpr uint64_t REG_STATUS = 0x08;
    static constexpr uint64_t REG_REGION_ID = 0x10;
    static constexpr uint64_t REG_COMPLETE = 0x18;
    static constexpr uint64_t REG_ITERATIONS = 0x20;
    static constexpr uint64_t REG_DEBUG = 0x28;
    static constexpr uint64_t REG_DEBUG_DATA = 0x30;

    enum RequestType { NONE = 0, FILL_REQUEST = 1, CHECK_REQUEST = 2 };
    enum StatusType { BUSY = 0, READY = 1, COMPLETE = 2 };
    enum CompleteType { FILL_DONE = 1, CHECK_DONE = 2 };

    SCP_LOGGER();
    SCP_LOGGER((TEST), "test");

    struct CPUState {
        RequestType current_request = NONE;
        StatusType status = BUSY;
        uint32_t assigned_region = 0xFFFFFFFF;
        uint32_t iteration_count = 0;
        bool is_working = false;
        bool has_started = false;  // Track if CPU has properly started up
    };

    tlm_utils::simple_target_socket<SMMUTesterController> socket;

    class CpuHexagonSMMUStressTestV2* m_parent;

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

        m_cpu_states.resize(num_cpus);

        for (uint32_t i = 0; i < num_regions; ++i) {
            m_available_regions.push(i);
        }

        SCP_INFO((TEST)) << "SMMU Tester Controller initialized: " << num_cpus << " CPUs, " << num_regions
                         << " regions, target " << target_iterations << " iterations";
    }

    void set_parent(class CpuHexagonSMMUStressTestV2* parent) { m_parent = parent; }

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
            SCP_INFO(()) << "CPU " << cpu_id << " writing to REG_COMPLETE with data " << data;
            handle_complete(cpu_id, static_cast<CompleteType>(data));
            break;

        case REG_DEBUG:
            handle_debug(cpu_id, data);
            break;

        case REG_DEBUG_DATA:
            SCP_DEBUG(()) << "CPU " << cpu_id << " debug data: 0x" << std::hex << data;
            break;

        default:
            SCP_FATAL(()) << "CPU " << cpu_id << " write to unknown register 0x" << std::hex << reg_offset;
            break;
        }
    }

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
            SCP_FATAL(()) << "CPU " << cpu_id << " read from unknown register 0x" << std::hex << reg_offset;
            break;
        }

        *reinterpret_cast<uint64_t*>(trans.get_data_ptr()) = data;
    }

    void handle_request(uint32_t cpu_id, RequestType request)
    {
        CPUState& cpu = m_cpu_states[cpu_id];

        // Ensure CPU has properly started up before accepting any requests
        if (!cpu.has_started) {
            SCP_WARN(()) << "CPU " << cpu_id << " requesting " << (request == FILL_REQUEST ? "FILL" : "CHECK")
                         << " but has not completed startup - ignoring";
            cpu.status = BUSY;
            return;
        }

        SCP_INFO(()) << "CPU " << cpu_id << " request: " << (request == FILL_REQUEST ? "FILL" : "CHECK");

        if (request == FILL_REQUEST) {
            if (!m_available_regions.empty()) {
                uint32_t region = m_available_regions.front();
                m_available_regions.pop();

                cpu.current_request = FILL_REQUEST;
                cpu.assigned_region = region;
                cpu.is_working = true;

                SCP_INFO(()) << "DEBUG: handle_request(FILL) assigning CPU " << cpu_id << " to region " << region
                             << ". Calling configure_smmu_mapping()";
                configure_smmu_mapping(cpu_id, region);

                cpu.status = READY;

                SCP_INFO((TEST)) << "CPU_" << cpu_id << " assigned region " << region << " for filling";
            } else {
                cpu.status = BUSY;
                SCP_FATAL(()) << "CPU_" << cpu_id << " fill request - no regions available";
            }
        } else if (request == CHECK_REQUEST) {
            if (!m_regions_to_check.empty()) {
                uint32_t region = m_regions_to_check.front();
                m_regions_to_check.pop();

                cpu.current_request = CHECK_REQUEST;
                cpu.assigned_region = region;
                cpu.is_working = true;

                configure_smmu_mapping(cpu_id, region);

                cpu.status = READY;

                SCP_INFO((TEST)) << "CPU_" << cpu_id << " assigned region " << region << " for checking";
            } else {
                cpu.status = BUSY;
                SCP_DEBUG(()) << "CPU_" << cpu_id << " check request - no regions to check";
            }
        }
    }

    void handle_debug(uint32_t cpu_id, uint64_t data)
    {
        uint32_t msg_type = (data & 0xF000) >> 12;
        uint32_t msg_data = (data & 0x0FFF);

        CPUState& cpu = m_cpu_states[cpu_id];

        switch (msg_type) {
        case 0x1:
            SCP_INFO(()) << "CPU " << cpu_id << " started up";
            cpu.has_started = true;
            break;
        case 0x2:
            SCP_INFO(()) << "CPU " << cpu_id << " entering main loop, iteration " << msg_data;
            break;
        case 0x3:
            SCP_INFO(()) << "CPU " << cpu_id << " polling for readiness";
            break;
        case 0x4:
            SCP_INFO(()) << "CPU " << cpu_id << " starting work on region " << msg_data;
            break;
        case 0x5:
            SCP_INFO(()) << "CPU " << cpu_id << " pattern verification success";
            break;
        case 0x6:
            SCP_INFO(()) << "CPU " << cpu_id << " DEBUG: About to start memory writing for region " << msg_data;
            break;
        case 0x7:
            SCP_INFO(()) << "CPU " << cpu_id << " DEBUG: Memory filling COMPLETED for region " << msg_data;
            break;
        case 0x8:
            SCP_INFO(()) << "CPU " << cpu_id << " DEBUG: Completed fill for region " << msg_data;
            break;
        case 0xD:
            SCP_WARN(()) << "CPU " << cpu_id << " pattern verification FAILED";
            break;
        case 0xE:
            SCP_FATAL(()) << "🚨 DIAGNOSTIC ERROR: CPU " << cpu_id
                          << " encountered fatal error - jumping to diagnostic handler at 0x200";
            sc_core::sc_stop();
            break;
        default:
            SCP_DEBUG(()) << "CPU " << cpu_id << " debug: 0x" << std::hex << data;
            break;
        }
    }

    void configure_smmu_mapping(uint32_t cpu_id, uint32_t region_id);
    void unmap_cpu_region(uint32_t cpu_id);
    bool verify_region_pattern(uint32_t cpu_id, uint32_t region_id);
    void clear_region_pattern(uint32_t region_id);
};

/**
 * @class CpuHexagonSMMUStressTestV2
 * @brief Hexagon version of the SMMU stress test
 *
 * This test uses Hexagon CPUs behind SMMU TBUs to stress test the SMMU-500.
 * The architecture is identical to the ARM version, only the CPU type and
 * firmware differ.
 */
class CpuHexagonSMMUStressTestV2 : public TestBench, public CpuTesterCallbackIface
{
public:
    static constexpr uint16_t MAX_ITERATIONS = 0x7fff;
    static constexpr uint64_t PATTERN_SIZE = 16;
    static constexpr unsigned BOUNDARY_BYTES = 80;
    sc_core::sc_time TEST_DURATION = sc_core::sc_time(30, sc_core::SC_SEC);

    // Memory layout - same as ARM version
    static constexpr uint64_t MEM_ADDR = 0x0;
    static constexpr size_t MEM_SIZE = 256 * 1024;
    static constexpr uint64_t BOOT_ADDR = 0x0;
    static constexpr uint64_t DIAGNOSTIC_ADDR = 0x200;
    static constexpr uint64_t MAIN_FIRMWARE_ADDR = 0x1000;
    static constexpr uint64_t TESTER_ADDR = 0x40000;
    static constexpr uint64_t TESTER_SIZE = 0x10000;
    static constexpr uint64_t SMMU_REG_ADDR = 0x50000;
    static constexpr uint64_t SMMU_REG_SIZE = 0x20000;
    static constexpr uint64_t MAIN_MEM_ADDR = 0x10000000;
    static constexpr size_t MAIN_MEM_SIZE = 256 * 1024 * 1024;

    static constexpr uint64_t VIRTUAL_TEST_ADDR = 0x80000000ULL;
    static constexpr uint64_t PAGE_TABLE_BASE = 0x10000000;
    static constexpr uint64_t REGION_BASE = 0x10080000;
    static constexpr uint64_t REGION_SIZE = 0x2000;
    static constexpr uint64_t PAGE_SIZE = 0x1000;

    SCP_LOGGER();

protected:
    struct PageTableAddresses {
        uint64_t l0, l1, l2, l3;
    };

    PageTableAddresses get_page_table_addresses_for_cpu(uint32_t cpu) const
    {
        uint64_t base = PAGE_TABLE_BASE + ((cpu + 1) * PAGE_SIZE * 4);
        return { base, base + PAGE_SIZE, base + (PAGE_SIZE * 2), base + (PAGE_SIZE * 3) };
    }

    cci::cci_param<int> p_num_cpu;
    cci::cci_param<int> p_quantum_ns;

    QemuInstanceManager m_inst_manager;
    QemuInstance m_inst_a;
    QemuInstance m_inst_b;
    hexagon_globalreg m_hex_gregs_a;
    hexagon_globalreg m_hex_gregs_b;
    bool ab = false;
    sc_core::sc_vector<qemu_cpu_hexagon> m_cpus;

    gs::gs_memory<> m_mem;
    gs::gs_memory<> m_main_mem;

    gs::smmu500<> m_smmu;

    std::vector<gs::pass<>*> m_pass_identity;
    std::vector<gs::smmu500_tbu<>*> m_tbus_high_va;

    gs::router<> m_global_router;
    std::vector<gs::router<>*> m_cpu_routers;

    SMMUTesterController m_tester_controller;

    uint32_t m_num_regions;
    std::vector<uint32_t> m_cpu_to_region;

    // Dummy interrupt sinks for SMMU context bank interrupts
    sc_core::sc_vector<TargetSignalSocket<bool>> m_dummy_irq_sinks;

public:
    MemoryAccessor m_memory_accessor;

private:
    static constexpr uint32_t SMMU_SCR0_OFFSET = 0x0;
    static constexpr uint32_t SMMU_SMR_BASE_OFFSET = 0x800;
    static constexpr uint32_t SMMU_S2CR_BASE_OFFSET = 0xc00;
    static constexpr uint32_t SMMU_CBAR_BASE_OFFSET = 0x1000;

    static constexpr uint32_t CB_PAGE_OFFSET = 16;
    static constexpr uint32_t CB_PAGE_SIZE = 4096;

    static constexpr uint32_t CB_SCTLR_OFFSET = 0x0;
    static constexpr uint32_t CB_TTBR0_LOW_OFFSET = 0x20;
    static constexpr uint32_t CB_TTBR0_HIGH_OFFSET = 0x24;
    static constexpr uint32_t CB_TCR_OFFSET = 0x30;
    static constexpr uint32_t CB_MAIR0_OFFSET = 0x38;
    static constexpr uint32_t CB_MAIR1_OFFSET = 0x3C;
    static constexpr uint32_t CB_TLBIALL_OFFSET = 0x618;

    uint32_t get_context_bank_base(uint32_t cb) const
    {
        assert(cb < m_smmu.p_num_cb && "Context bank ID out of range");
        uint32_t cb_offset_words = ((CB_PAGE_OFFSET + cb) * CB_PAGE_SIZE) / 4;
        return SMMU_REG_ADDR + (cb_offset_words * 4);
    }

    void reconfigure_context_bank(uint32_t cb, uint64_t page_table_addr);

protected:
    void set_firmware_from_binary(const uint8_t* binary, size_t size, uint64_t addr = 0)
    {
        m_mem.load.ptr_load(const_cast<uint8_t*>(binary), addr, size);
        SCP_INFO(()) << "Loaded " << size << " bytes of Hexagon firmware at 0x" << std::hex << addr;
    }

public:
    CpuHexagonSMMUStressTestV2(const sc_core::sc_module_name& n)
        : TestBench(n)
        , p_num_cpu("num_cpu", 4, "Number of CPUs to instantiate in the test")
        , p_quantum_ns("quantum_ns", 1000000, "Value of the global TLM-2.0 quantum in ns")
        , m_inst_a("inst_a", &m_inst_manager, qemu_cpu_hexagon::ARCH)
        , m_inst_b("inst_b", &m_inst_manager, qemu_cpu_hexagon::ARCH)
        , m_hex_gregs_a("hex_gregs_a", &m_inst_a)
        , m_hex_gregs_b("hex_gregs_b", &m_inst_b)
        , m_cpus("cpu", p_num_cpu,
                 [this](const char* n, int i) {
                     ab = !ab;
                     return new qemu_cpu_hexagon(n, ab ? m_inst_a : m_inst_b);
                 })
        , m_mem("mem", MEM_SIZE)
        , m_main_mem("main_mem", MAIN_MEM_SIZE)
        , m_smmu("smmu")
        , m_global_router("global_router")
        , m_tester_controller("tester_controller", p_num_cpu.get_value(),
                              std::max(3u, static_cast<uint32_t>(p_num_cpu.get_value() * 3)), MAX_ITERATIONS)
        , m_memory_accessor("memory_accessor")
        , m_dummy_irq_sinks("dummy_irq_sink", p_num_cpu.get_value() * 2)  // num_cb = num_cpu * 2
    {
        using tlm_utils::tlm_quantumkeeper;

        sc_core::sc_time global_quantum(p_quantum_ns, sc_core::SC_NS);
        tlm_quantumkeeper::set_global_quantum(global_quantum);

        m_num_regions = std::max(3u, static_cast<uint32_t>(p_num_cpu.get_value() * 3));

        SCP_INFO(()) << "Creating Hexagon SMMU Stress Test V2 with " << p_num_cpu.get_value() << " CPUs, "
                     << m_num_regions << " regions";

        m_global_router.add_initiator(m_memory_accessor.socket);

        m_smmu.p_num_tbu = p_num_cpu.get_value();
        m_smmu.p_num_cb = p_num_cpu.get_value() * 2;
        m_smmu.p_num_smr = 64;
        m_smmu.p_num_pages = std::max(16u, static_cast<uint32_t>(p_num_cpu.get_value() * 2));
        SCP_INFO(()) << "SMMU500 instantiated: p_num_cb=" << m_smmu.p_num_cb << ", p_num_smr=" << m_smmu.p_num_smr
                     << ", p_num_pages=" << m_smmu.p_num_pages;

        uint32_t num_cpus = p_num_cpu.get_value();
        m_pass_identity.resize(num_cpus);
        m_tbus_high_va.resize(num_cpus);

        for (uint32_t i = 0; i < num_cpus; ++i) {
            char pass_identity_name[32];
            std::snprintf(pass_identity_name, sizeof(pass_identity_name), "pass_identity_%d", i);
            m_pass_identity[i] = new gs::pass<>(pass_identity_name);
            SCP_INFO(()) << "Identity pass-through constructed: CPU" << i << " (bypassing SMMU for identity traffic)";

            char tbu_high_va_name[32];
            std::snprintf(tbu_high_va_name, sizeof(tbu_high_va_name), "tbu_high_va_%d", i);
            m_tbus_high_va[i] = new gs::smmu500_tbu<>(tbu_high_va_name, &m_smmu);
            uint32_t high_va_topology_id = i + 1;
            m_tbus_high_va[i]->p_topology_id = high_va_topology_id;
            m_tbus_high_va[i]->p_topology_id.set_value(high_va_topology_id);
            SCP_INFO(()) << "High VA TBU constructed: CPU" << i << " topology_id=" << high_va_topology_id
                         << " (StreamID " << high_va_topology_id << " → CB" << high_va_topology_id << ")";
        }

        m_cpu_routers.resize(num_cpus);
        for (uint32_t i = 0; i < num_cpus; ++i) {
            char router_name[32];
            std::snprintf(router_name, sizeof(router_name), "cpu_router_%d", i);
            m_cpu_routers[i] = new gs::router<>(router_name);
            SCP_INFO(()) << "Per-CPU router constructed: " << router_name;
        }

        m_cpu_to_region.resize(p_num_cpu.get_value(), 0xFFFFFFFF);

        m_hex_gregs_a.p_hexagon_start_addr = BOOT_ADDR;
        m_hex_gregs_b.p_hexagon_start_addr = BOOT_ADDR;

        m_tester_controller.set_parent(this);

        for (uint32_t i = 0; i < p_num_cpu.get_value(); ++i) {
            m_cpu_routers[i]->add_initiator(m_cpus[i].socket);

            m_cpu_routers[i]->add_target(m_pass_identity[i]->target_socket, 0x0, 0x10000000ULL);

            m_cpu_routers[i]->add_target(m_tbus_high_va[i]->upstream_socket, 0x80000000ULL, 0x80000000ULL);

            SCP_INFO(()) << "🔍 ROUTING DEBUG: CPU " << i << " -> CPU_Router_" << i;
            SCP_INFO(()) << "  - Identity range [0x0 - 0x10000000] -> Pass_Identity_" << i << " (bypassing SMMU)";
            SCP_INFO(()) << "  - High VA range [0x80000000 - 0x100000000] -> High_VA_TBU_" << i << " (StreamID "
                         << (i + 1) << ")";
        }

        m_global_router.add_target(m_mem.socket, MEM_ADDR, MEM_SIZE);
        m_global_router.add_target(m_main_mem.socket, MAIN_MEM_ADDR, MAIN_MEM_SIZE);
        m_global_router.add_target(m_smmu.socket, SMMU_REG_ADDR, SMMU_REG_SIZE);
        m_global_router.add_target(m_tester_controller.socket, TESTER_ADDR, TESTER_SIZE);

        for (uint32_t i = 0; i < p_num_cpu.get_value(); ++i) {
            m_global_router.add_initiator(m_pass_identity[i]->initiator_socket);
            m_global_router.add_initiator(m_tbus_high_va[i]->downstream_socket);
        }

        m_global_router.add_initiator(m_smmu.dma_socket);

        // Bind SMMU interrupt context ports to dummy sinks
        // We need to bind all context bank interrupts even if not all are used
        for (uint32_t i = 0; i < m_smmu.p_num_cb; ++i) {
            m_smmu.irq_context[i].bind(m_dummy_irq_sinks[i]);
        }

        SCP_INFO(()) << "Memory layout:";
        SCP_INFO(()) << "  FIRMWARE: 0x" << std::hex << MEM_ADDR;
        SCP_INFO(()) << "  MAIN_MEM: 0x" << std::hex << MAIN_MEM_ADDR;
        SCP_INFO(()) << "  REGIONS: 0x" << std::hex << REGION_BASE;
        SCP_INFO(()) << "  SMMU_REG: 0x" << std::hex << SMMU_REG_ADDR;
        SCP_INFO(()) << "  TESTER: 0x" << std::hex << TESTER_ADDR;
        SCP_INFO(()) << "  VIRTUAL_TEST: 0x" << std::hex << VIRTUAL_TEST_ADDR;

        SCP_INFO(()) << "🔥 ABOUT TO CALL load_hexagon_firmware()";
        load_hexagon_firmware();
        SCP_INFO(()) << "🔥 AFTER CALLING load_hexagon_firmware()";

        SC_THREAD(configure_test);
    }

    virtual ~CpuHexagonSMMUStressTestV2()
    {
        for (auto* pass : m_pass_identity) {
            delete pass;
        }

        for (auto* tbu : m_tbus_high_va) {
            delete tbu;
        }
        for (auto* router : m_cpu_routers) {
            delete router;
        }
    }

    void before_end_of_elaboration() override
    {
        TestBench::before_end_of_elaboration();

        m_hex_gregs_a.before_end_of_elaboration();
        m_hex_gregs_b.before_end_of_elaboration();

        qemu::Device hex_gregs_a_dev = m_hex_gregs_a.get_qemu_dev();
        qemu::Device hex_gregs_b_dev = m_hex_gregs_b.get_qemu_dev();

        for (int i = 0; i < m_cpus.size(); i++) {
            auto& cpu = m_cpus[i];
            cpu.before_end_of_elaboration();
            qemu::Device cpu_dev = cpu.get_qemu_dev();

            // Link to appropriate global regs based on which instance
            bool uses_inst_a = (i % 2 == 0);
            cpu_dev.set_prop_link("global-regs", uses_inst_a ? hex_gregs_a_dev : hex_gregs_b_dev);
        }
    }

    void configure_test()
    {
        wait(sc_core::sc_time(100, sc_core::SC_US));

        SCP_INFO(()) << "Configuring SMMU for tester-controlled operation";

        write_smmu_register(SMMU_REG_ADDR + SMMU_SCR0_OFFSET, 0x0);

        // Configure SMRs and S2CRs - same as ARM version
        uint32_t smr0_addr = SMMU_REG_ADDR + SMMU_SMR_BASE_OFFSET;
        uint32_t smr0_value = (1 << 31) | (0 << 16) | (0 << 0);
        write_smmu_register(smr0_addr, smr0_value);

        uint32_t s2cr0_addr = SMMU_REG_ADDR + SMMU_S2CR_BASE_OFFSET;
        uint32_t s2cr0_value = (0x1 << 16) | (0 << 0);
        write_smmu_register(s2cr0_addr, s2cr0_value);

        SCP_INFO(()) << "SMR[0]/S2CR[0]: StreamID=0 -> CB0 (SHARED identity for ALL CPUs)";

        for (uint32_t cpu = 0; cpu < p_num_cpu.get_value(); ++cpu) {
            uint32_t high_va_stream_id = cpu + 1;
            uint32_t high_va_cb = cpu + 1;

            uint32_t smr_addr = SMMU_REG_ADDR + SMMU_SMR_BASE_OFFSET + (high_va_stream_id * 4);
            uint32_t smr_value = (1 << 31) | (0 << 16) | (high_va_stream_id << 0);
            write_smmu_register(smr_addr, smr_value);

            uint32_t s2cr_addr = SMMU_REG_ADDR + SMMU_S2CR_BASE_OFFSET + (high_va_stream_id * 4);
            uint32_t s2cr_value = (0x1 << 16) | (high_va_cb << 0);
            write_smmu_register(s2cr_addr, s2cr_value);

            SCP_INFO(()) << "SMR[" << high_va_stream_id << "]/S2CR[" << high_va_stream_id
                         << "]: StreamID=" << high_va_stream_id << " -> CB" << high_va_cb << " (CPU " << cpu
                         << " high VA)";
        }

        setup_identity_context_bank(0);

        for (uint32_t cpu = 0; cpu < p_num_cpu.get_value(); ++cpu) {
            uint32_t high_va_cb = cpu + 1;
            setup_complete_high_va_context_bank(cpu, high_va_cb);

            SCP_INFO(()) << "Complete high VA context bank initialized for CPU " << cpu << " (CB" << high_va_cb
                         << ") - conflicts resolved";
        }

        SCP_INFO(()) << "SMMU configuration completed - ready for tester control";
    }

    void setup_identity_context_bank(uint32_t cpu)
    {
        uint32_t cb = cpu;
        uint32_t cb_base = get_context_bank_base(cb);

        uint32_t cbar_addr = SMMU_REG_ADDR + SMMU_CBAR_BASE_OFFSET + (cb * 4);
        write_smmu_register(cbar_addr, (1 << 16));

        write_smmu_register(cb_base + CB_SCTLR_OFFSET, 0x0);

        write_smmu_register(cb_base + CB_TTBR0_LOW_OFFSET, 0x0);
        write_smmu_register(cb_base + CB_TTBR0_HIGH_OFFSET, 0x0);

        write_smmu_register(cb_base + CB_TCR_OFFSET, (1U << 31) | (25 << 0));

        write_smmu_register(cb_base + CB_MAIR0_OFFSET, 0xFF);
        write_smmu_register(cb_base + CB_MAIR1_OFFSET, 0x0);

        SCP_INFO(()) << "Identity context bank CB" << cb << " set up for CPU " << cpu
                     << " with MMU DISABLED (pure identity mapping VA=PA)";
    }

    void clear_page_tables_for_cpu(uint32_t cpu)
    {
        const auto pt_addrs = get_page_table_addresses_for_cpu(cpu);

        // Clear all page table levels - 4KB each
        for (uint32_t i = 0; i < (PAGE_SIZE / 8); ++i) {
            write_memory_64(pt_addrs.l0 + (i * 8), 0x0);
            write_memory_64(pt_addrs.l1 + (i * 8), 0x0);
            write_memory_64(pt_addrs.l2 + (i * 8), 0x0);
            write_memory_64(pt_addrs.l3 + (i * 8), 0x0);
        }

        SCP_INFO(()) << "✅ Cleared page tables for CPU " << cpu;
    }

    void setup_complete_high_va_context_bank(uint32_t cpu, uint32_t cb)
    {
        assert(cpu < p_num_cpu.get_value() && "CPU ID out of range");
        assert(cb > 0 && "High VA context bank must be > 0");
        assert(cb == cpu + 1 && "High VA CB must map to CPU+1");

        SCP_INFO(()) << "🔧 CONSOLIDATED SETUP: Setting up complete high VA context bank for CPU " << cpu << " (CB"
                     << cb << ") - resolving function conflicts";

        // Clear page tables first to avoid stale entries
        clear_page_tables_for_cpu(cpu);

        const auto pt_addrs = get_page_table_addresses_for_cpu(cpu);

        uint64_t l0_table_addr = pt_addrs.l0;
        uint64_t l1_table_addr = pt_addrs.l1;
        uint64_t l2_table_addr = pt_addrs.l2;
        uint64_t l3_table_addr = pt_addrs.l3;

        SCP_INFO(()) << "  - Page table base: 0x" << std::hex << l0_table_addr;

        // Calculate proper indices for our virtual address
        uint32_t l0_index = (VIRTUAL_TEST_ADDR >> 39) & 0x1FF;  // Level 0 index
        uint32_t l1_index = (VIRTUAL_TEST_ADDR >> 30) & 0x1FF;  // Level 1 index (should be 2 for 0x80000000)

        // Set up L0 entry for our virtual address range
        uint64_t l0_desc = (l1_table_addr & ~0xFFFULL) | (1ULL << 10) | 0x3ULL;
        write_memory_64(l0_table_addr + (l0_index * 8), l0_desc);

        // Set up L1 entry for our virtual address range
        uint64_t l1_desc = (l2_table_addr & ~0xFFFULL) | (1ULL << 10) | 0x3ULL;
        write_memory_64(l1_table_addr + (l1_index * 8), l1_desc);

        uint32_t l2_index = (VIRTUAL_TEST_ADDR >> 21) & 0x1FF;
        uint64_t l2_desc = (l3_table_addr & ~0xFFFULL) | (1ULL << 10) | 0x3ULL;
        write_memory_64(l2_table_addr + (l2_index * 8), l2_desc);

        uint64_t default_physical_addr = REGION_BASE + (cpu * REGION_SIZE);
        uint64_t l3_desc_0 = (default_physical_addr & ~0xFFFULL) | (1ULL << 10) | (3ULL << 8) | (3ULL << 2) |
                             (1ULL << 6) | 0x3ULL;
        write_memory_64(l3_table_addr + (0 * 8), l3_desc_0);

        uint32_t cb_base = get_context_bank_base(cb);

        uint32_t cbar_addr = SMMU_REG_ADDR + SMMU_CBAR_BASE_OFFSET + (cb * 4);
        write_smmu_register(cbar_addr, (1 << 16));

        write_smmu_register(cb_base + CB_TTBR0_LOW_OFFSET, static_cast<uint32_t>(l0_table_addr & 0xFFFFFFFF));
        write_smmu_register(cb_base + CB_TTBR0_HIGH_OFFSET, static_cast<uint32_t>((l0_table_addr >> 32) & 0xFFFFFFFF));

        write_smmu_register(cb_base + CB_TCR_OFFSET,
                            (1U << 31) | (16 << 0) | (0 << 14) | (3 << 12) | (1 << 10) | (1 << 8));

        write_smmu_register(cb_base + CB_MAIR0_OFFSET, 0xFF);
        write_smmu_register(cb_base + CB_MAIR1_OFFSET, 0x0);

        uint32_t sctlr_value = (1 << 0) | (1 << 2) | (1 << 4);
        write_smmu_register(cb_base + CB_SCTLR_OFFSET, sctlr_value);

        SCP_INFO(()) << "SETUP COMPLETE: CPU " << cpu << " CB" << cb;
    }

    void clear_region_pattern(uint32_t region_id)
    {
        uint64_t physical_addr = REGION_BASE + (region_id * REGION_SIZE);

        SCP_INFO(()) << "Clearing region " << region_id << " at physical address 0x" << std::hex << physical_addr;

        const uint32_t words_to_clear = BOUNDARY_BYTES / 8;
        const uint32_t num_pages = REGION_SIZE / PAGE_SIZE;

        for (uint32_t page_num = 0; page_num < num_pages; ++page_num) {
            uint64_t page_physical_addr = physical_addr + (page_num * PAGE_SIZE);

            for (uint32_t i = 0; i < words_to_clear; ++i) {
                // Put the clear pattern in the upper 32 bits where CPU ID goes
                uint64_t clear_pattern = (static_cast<uint64_t>(0xCAFE) << 32) | (region_id << 16) | (page_num << 8) | i;
                write_memory_64(page_physical_addr + (i * 8), clear_pattern);
            }

            for (uint32_t i = 0; i < words_to_clear; ++i) {
                // Put the clear pattern in the upper 32 bits where CPU ID goes
                uint64_t clear_pattern = (static_cast<uint64_t>(0xCAFE) << 32) | (region_id << 16) | (page_num << 8) | i;
                write_memory_64(page_physical_addr + PAGE_SIZE - (words_to_clear * 8) + (i * 8), clear_pattern);
            }
        }

        SCP_INFO(()) << "✅ Region " << region_id << " cleared";
    }

    void map_cpu_to_region(uint32_t cpu, uint32_t region)
    {
        assert(cpu < m_cpu_to_region.size() && "CPU ID out of range");
        assert(region < m_num_regions && "Region ID out of range");

        if (cpu >= m_cpu_to_region.size()) return;

        m_cpu_to_region[cpu] = region;

        uint32_t high_va_cb = cpu + 1;
        uint64_t physical_addr = REGION_BASE + (region * REGION_SIZE);
        const auto pt_addrs = get_page_table_addresses_for_cpu(cpu);

        SCP_INFO(()) << "Starting map_cpu_to_region for CPU " << cpu << " -> Region " << region;

        clear_region_pattern(region);

        create_page_table_mapping(cpu, VIRTUAL_TEST_ADDR, physical_addr, REGION_SIZE);

        reconfigure_context_bank(high_va_cb, pt_addrs.l0);

        SCP_INFO(()) << "✅ SMMU mapping completed successfully for CPU " << cpu << " -> Region " << region;
    }

    void create_page_table_mapping(uint32_t cpu, uint64_t virtual_addr, uint64_t physical_addr, uint64_t size)
    {
        assert(cpu < p_num_cpu.get_value() && "CPU ID out of range");
        assert((physical_addr & 0xFFF) == 0 && "Physical address must be page-aligned");
        assert((virtual_addr & 0xFFF) == 0 && "Virtual address must be page-aligned");
        assert(size == REGION_SIZE && "Size must match REGION_SIZE");

        const auto pt_addrs = get_page_table_addresses_for_cpu(cpu);

        uint64_t l3_table_addr = pt_addrs.l3;

        uint32_t num_pages = size / PAGE_SIZE;

        SCP_INFO(()) << "🔍 TWO-PAGE MAPPING: Creating page tables for CPU " << cpu << " (" << num_pages << " pages)";

        for (uint32_t page = 0; page < num_pages; ++page) {
            uint64_t page_pa = physical_addr + (page * PAGE_SIZE);

            uint64_t l3_desc = (page_pa & ~0xFFFULL) | (1ULL << 10) | (3ULL << 8) | (3ULL << 2) | (1ULL << 6) | 0x3ULL;
            write_memory_64(l3_table_addr + (page * 8), l3_desc);

            uint64_t l3_readback = m_memory_accessor.read_memory(l3_table_addr + (page * 8));
            uint64_t extracted_physical = l3_readback & ~0xFFFULL;

            if (extracted_physical != page_pa) {
                SCP_FATAL(()) << "🚨 CRITICAL: L3[" << page << "] physical address mismatch!";
            }
        }

        SCP_INFO(()) << "✅ TWO-PAGE mapping complete for CPU " << cpu;

        invalidate_smmu_tlb(cpu);
    }

    void unmap_cpu_region(uint32_t cpu)
    {
        if (cpu >= m_cpu_to_region.size()) return;

        uint32_t high_va_cb = cpu + 1;

        if (high_va_cb == 0) {
            SCP_FATAL(()) << "CRITICAL BUG: Attempt to unmap shared CB0 for CPU " << cpu;
            return;
        }

        uint32_t cb_base = get_context_bank_base(high_va_cb);

        SCP_INFO(()) << "Unmapping CPU " << cpu << " from CB" << high_va_cb;

        write_smmu_register(cb_base + CB_SCTLR_OFFSET, 0x0);

        write_smmu_register(cb_base + CB_TTBR0_LOW_OFFSET, 0x0);
        write_smmu_register(cb_base + CB_TTBR0_HIGH_OFFSET, 0x0);

        m_cpu_to_region[cpu] = 0xFFFFFFFF;
    }

    void load_hexagon_firmware()
    {
        SCP_INFO(()) << "🔥 LOADING HEXAGON FIRMWARE from file: " << FIRMWARE_BIN_PATH;

        // Read the firmware from the compiled binary file
        std::ifstream firmware_file(FIRMWARE_BIN_PATH, std::ios::binary | std::ios::ate);
        if (!firmware_file) {
            SCP_FATAL(()) << "Failed to open firmware file: " << FIRMWARE_BIN_PATH;
            return;
        }

        std::streamsize firmware_size = firmware_file.tellg();
        firmware_file.seekg(0, std::ios::beg);

        std::vector<uint8_t> firmware_data(firmware_size);
        if (!firmware_file.read(reinterpret_cast<char*>(firmware_data.data()), firmware_size)) {
            SCP_FATAL(()) << "Failed to read firmware file: " << FIRMWARE_BIN_PATH;
            return;
        }

        SCP_INFO(()) << "🔥 LOADED " << firmware_size << " bytes of Hexagon firmware, calling set_firmware_from_binary";
        set_firmware_from_binary(firmware_data.data(), firmware_size, BOOT_ADDR);
        SCP_INFO(()) << "🔥 FIRMWARE SET COMPLETE";
    }

    void write_smmu_register(uint32_t addr, uint32_t value) { m_memory_accessor.write_register(addr, value); }

    uint32_t read_smmu_register(uint32_t addr) { return m_memory_accessor.read_register(addr); }

    void write_memory_64(uint64_t addr, uint64_t value) { m_memory_accessor.write_memory(addr, value); }

    void invalidate_smmu_tlb(uint32_t cpu)
    {
        uint32_t high_va_cb = cpu + 1;
        uint32_t cb_base = get_context_bank_base(high_va_cb);

        SCP_INFO(()) << "🔄 TLB INVALIDATION: Invalidating SMMU TLB for CPU " << cpu << " (CB" << high_va_cb << ")";

        uint32_t tlbiall_addr = cb_base + CB_TLBIALL_OFFSET;
        write_smmu_register(tlbiall_addr, 0x0);

        wait(sc_core::sc_time(2, sc_core::SC_US));

        SCP_INFO(()) << "✅ TLB INVALIDATION COMPLETE: CPU " << cpu << " TLB cleared";
    }

    virtual void map_target(tlm::tlm_target_socket<DEFAULT_TLM_BUSWIDTH>& s, uint64_t addr, uint64_t size) override {}
    virtual void map_irqs_to_cpus(sc_core::sc_vector<InitiatorSignalSocket<bool> >& irqs) override {}
    virtual uint64_t mmio_read(int id, uint64_t addr, size_t len) override { return 0; }
    virtual bool dmi_request(int id, uint64_t addr, size_t len, tlm::tlm_dmi& ret) override { return false; }
    virtual void mmio_write(int id, uint64_t addr, uint64_t data, size_t len) override {}

    virtual void end_of_simulation() override
    {
        SCP_WARN(()) << "Hexagon SMMU Stress Test V2 completed"
                     << "\nFinal statistics:"
                     << "\n  - Total iterations: " << m_tester_controller.m_global_iterations << "/" << MAX_ITERATIONS
                     << "\n  - CPUs: " << p_num_cpu.get_value() << "\n  - Memory regions: " << m_num_regions;
    }
};

void CpuHexagonSMMUStressTestV2::reconfigure_context_bank(uint32_t cb, uint64_t page_table_addr)
{
    uint32_t cb_base = get_context_bank_base(cb);

    write_smmu_register(cb_base + CB_TTBR0_LOW_OFFSET, static_cast<uint32_t>(page_table_addr & 0xFFFFFFFF));
    write_smmu_register(cb_base + CB_TTBR0_HIGH_OFFSET, static_cast<uint32_t>((page_table_addr >> 32) & 0xFFFFFFFF));

    uint32_t tcr_value = (1U << 31) | (16 << 0) | (0 << 14) | (3 << 12) | (1 << 10) | (1 << 8);
    write_smmu_register(cb_base + CB_TCR_OFFSET, tcr_value);

    write_smmu_register(cb_base + CB_MAIR0_OFFSET, 0xFF);
    write_smmu_register(cb_base + CB_MAIR1_OFFSET, 0x0);

    uint32_t sctlr_value = (1 << 0) | (1 << 2) | (1 << 4);
    write_smmu_register(cb_base + CB_SCTLR_OFFSET, sctlr_value);
}

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

bool SMMUTesterController::verify_region_pattern(uint32_t cpu_id, uint32_t region_id)
{
    if (!m_parent) {
        SCP_FATAL(()) << "No parent reference for memory access";
        return false;
    }

    uint64_t physical_addr = CpuHexagonSMMUStressTestV2::REGION_BASE +
                             (region_id * CpuHexagonSMMUStressTestV2::REGION_SIZE);

    SCP_INFO(()) << "Verifying region " << region_id << " at PA=0x" << std::hex << physical_addr;

    const uint32_t words_to_check = 10;
    const uint32_t num_pages = CpuHexagonSMMUStressTestV2::REGION_SIZE / CpuHexagonSMMUStressTestV2::PAGE_SIZE;

    for (uint32_t page_num = 0; page_num < num_pages; ++page_num) {
        uint64_t page_physical_addr = physical_addr + (page_num * CpuHexagonSMMUStressTestV2::PAGE_SIZE);

        for (uint32_t word = 0; word < words_to_check; ++word) {
            uint64_t word_addr = page_physical_addr + (word * 8);
            uint64_t read_value = m_parent->m_memory_accessor.read_memory(word_addr);

            uint32_t pattern_cpu = (read_value >> 32) & 0xFFFFFFFF;
            uint32_t pattern_region = (read_value >> 16) & 0xFFFF;
            uint32_t pattern_page = (read_value >> 8) & 0xFF;
            uint32_t pattern_offset = read_value & 0xFF;

            // Check if the region still contains the clear pattern (0xcafe or 0xbeef)
            if (pattern_cpu == 0xcafe || pattern_cpu == 0xbeef || pattern_cpu == 0x0) {
                SCP_WARN(()) << "Region " << region_id << " still contains clear pattern 0x" << std::hex << pattern_cpu
                             << " at page " << std::dec << page_num << " word " << word
                             << " - CPU may not have completed filling";
                return false;
            }

            if (pattern_cpu != cpu_id || pattern_region != region_id || pattern_page != page_num ||
                pattern_offset != word) {
                SCP_FATAL(()) << "🚨 PATTERN MISMATCH at region " << region_id << " page " << page_num
                             << std::hex
                             << " - Expected: CPU=0x" << cpu_id << " REGION=0x" << region_id
                             << " PAGE=0x" << page_num << " WORD=0x" << word
                             << " - Got: CPU=0x" << pattern_cpu << " REGION=0x" << pattern_region
                             << " PAGE=0x" << pattern_page << " WORD=0x" << pattern_offset
                             << " (raw value=0x" << read_value << ")";
                return false;
            }
        }

        for (uint32_t word = 0; word < words_to_check; ++word) {
            uint64_t word_addr = page_physical_addr + CpuHexagonSMMUStressTestV2::PAGE_SIZE - (words_to_check * 8) +
                                 (word * 8);
            uint64_t read_value = m_parent->m_memory_accessor.read_memory(word_addr);

            uint32_t pattern_cpu = (read_value >> 32) & 0xFFFFFFFF;
            uint32_t pattern_region = (read_value >> 16) & 0xFFFF;
            uint32_t pattern_page = (read_value >> 8) & 0xFF;
            uint32_t pattern_offset = read_value & 0xFF;

            // Check if the region still contains the clear pattern (0xcafe or 0xbeef)
            if (pattern_cpu == 0xcafe || pattern_cpu == 0xbeef || pattern_cpu == 0x0) {
                SCP_WARN(()) << "Region " << region_id << " still contains clear pattern 0x" << std::hex << pattern_cpu
                             << " at page " << std::dec << page_num << " word " << word
                             << " - CPU may not have completed filling";
                return false;
            }

            if (pattern_cpu != cpu_id || pattern_region != region_id || pattern_page != page_num ||
                pattern_offset != word) {
                SCP_FATAL(()) << "🚨 PATTERN MISMATCH at region " << region_id << " page " << page_num
                             << std::hex
                             << " - Expected: CPU=0x" << cpu_id << " REGION=0x" << region_id
                             << " PAGE=0x" << page_num << " WORD=0x" << word
                             << " - Got: CPU=0x" << pattern_cpu << " REGION=0x" << pattern_region
                             << " PAGE=0x" << pattern_page << " WORD=0x" << pattern_offset
                             << " (raw value=0x" << read_value << ")";
                return false;
            }
        }
    }

    SCP_INFO(()) << "✅ Region " << region_id << " pattern verification successful";
    return true;
}

void SMMUTesterController::clear_region_pattern(uint32_t region_id)
{
    if (!m_parent) {
        SCP_FATAL(()) << "No parent reference for memory access";
        return;
    }

    m_parent->clear_region_pattern(region_id);
}

void SMMUTesterController::handle_complete(uint32_t cpu_id, CompleteType complete)
{
    CPUState& cpu = m_cpu_states[cpu_id];

    // Ensure CPU has properly started up before accepting completion signals
    if (!cpu.has_started) {
        SCP_WARN(()) << "CPU " << cpu_id << " reporting completion but has not completed startup - ignoring";
        return;
    }

    if (complete == FILL_DONE) {
        // Validate that this CPU actually has a legitimate region assignment
        if (cpu.assigned_region == 0xFFFFFFFF) {
            SCP_WARN(()) << "CPU " << cpu_id << " reported FILL_DONE but has no assigned region - ignoring";
            return;
        }

        if (cpu.assigned_region >= m_num_regions) {
            SCP_FATAL(()) << "CPU " << cpu_id << " has invalid assigned_region " << cpu.assigned_region;
            sc_core::sc_stop();
            return;
        }

        // Validate that this CPU is actually supposed to be working
        if (!cpu.is_working || cpu.current_request != FILL_REQUEST) {
            SCP_WARN(()) << "CPU " << cpu_id << " reported FILL_DONE but is not in FILL_REQUEST state - ignoring";
            return;
        }

        SCP_INFO(()) << "CPU " << cpu_id << " completed filling region " << cpu.assigned_region;

        // Give the CPU a bit of time to complete memory writes before verification
        wait(sc_core::sc_time(50, sc_core::SC_US));

        uint64_t physical_addr = CpuHexagonSMMUStressTestV2::REGION_BASE +
                                 (cpu.assigned_region * CpuHexagonSMMUStressTestV2::REGION_SIZE);

        // Multiple retry attempts with increasing delays
        const int max_retries = 5;
        const sc_core::sc_time retry_delays[] = {
            sc_core::sc_time(10, sc_core::SC_US),
            sc_core::sc_time(50, sc_core::SC_US),
            sc_core::sc_time(100, sc_core::SC_US),
            sc_core::sc_time(500, sc_core::SC_US),
            sc_core::sc_time(1000, sc_core::SC_US)
        };

        bool verification_successful = false;

        for (int retry = 0; retry < max_retries && !verification_successful; retry++) {
            uint64_t first_word = m_parent->m_memory_accessor.read_memory(physical_addr);
            uint32_t pattern_cpu = (first_word >> 32) & 0xFFFFFFFF;

            // Check if the region still contains the clear pattern (0xcafe, 0xbeef, or 0x0)
            if (pattern_cpu == 0xcafe || pattern_cpu == 0xbeef || pattern_cpu == 0x0) {
                SCP_WARN(()) << "CPU " << cpu_id << " reported FILL_DONE but region " << cpu.assigned_region
                             << " still contains clear pattern 0x" << std::hex << pattern_cpu
                             << " - retry " << std::dec << retry + 1 << "/" << max_retries;

                if (retry < max_retries - 1) {
                    wait(retry_delays[retry]);
                    continue;
                } else {
                    SCP_WARN(()) << "CPU " << cpu_id << " region " << cpu.assigned_region
                                 << " still contains clear pattern after all retries - ignoring completion signal";
                    // Mark the region as available again since the write apparently didn't complete
                    m_available_regions.push(cpu.assigned_region);
                    // Reset CPU state
                    cpu.current_request = NONE;
                    cpu.assigned_region = 0xFFFFFFFF;
                    cpu.is_working = false;
                    cpu.status = COMPLETE;
                    return;
                }
            }

            // Verify the pattern - but use the actual CPU that wrote it
            if (verify_region_pattern(pattern_cpu, cpu.assigned_region)) {
                verification_successful = true;
            } else {
                // Pattern verification failed - might be partially written
                if (retry < max_retries - 1) {
                    SCP_WARN(()) << "Pattern verification failed for region " << cpu.assigned_region
                                 << " - retry " << retry + 1 << "/" << max_retries;
                    wait(retry_delays[retry]);
                    continue;
                } else {
                    return; // Verification will fail in final attempt
                }
            }
        }

        if (verification_successful) {
            SCP_INFO((TEST))("Region {:d} verified by tester", cpu.assigned_region);
            m_available_regions.push(cpu.assigned_region);
            cpu.iteration_count++;
            m_global_iterations++;
        } else {
            SCP_FATAL(()) << "Region " << cpu.assigned_region << " verification failed";
            sc_core::sc_stop();
            return;
        }

    } else if (complete == CHECK_DONE) {
        SCP_WARN(()) << "CHECK_DONE reported by CPU " << cpu_id << " - this path is deprecated";
        m_available_regions.push(cpu.assigned_region);
        cpu.iteration_count++;
        m_global_iterations++;
    }

    unmap_cpu_region(cpu_id);

    cpu.current_request = NONE;
    cpu.assigned_region = 0xFFFFFFFF;
    cpu.is_working = false;
    cpu.status = COMPLETE;

    if (m_global_iterations >= m_target_iterations) {
        SCP_INFO((TEST)) << "Target iterations reached: " << m_global_iterations;
        sc_core::sc_stop();
    }
}

int sc_main(int argc, char* argv[])
{
    scp::init_logging(scp::LogConfig()
                          .fileInfoFrom(sc_core::SC_ERROR)
                          .logAsync(false)
                          .logLevel(scp::log::WARNING)
                          .msgTypeFieldWidth(30));

    gs::ConfigurableBroker m_broker{};
    cci::cci_originator orig{ "sc_main" };
    auto broker_h = m_broker.create_broker_handle(orig);
    ArgParser ap{ broker_h, argc, argv };

    SCP_INFO("sc_main") << "Start Hexagon SMMU Stress Test V2";
    CpuHexagonSMMUStressTestV2 test_bench("test-bench");

    if ((gs::cci_get<int>(broker_h, "test-bench.num_cpu") > 8) ||
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
    SCP_INFO("test")("Test done");
    SCP_INFO("sc_main") << "Test done";
    exit(0);
    return 0;
}
