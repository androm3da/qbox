/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <chrono>
#include <gtest/gtest.h>
#include <router.h>
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>
#include <scp/report.h> // For SCP_INFO, SCP_DEBUG etc.

using gs::router;
using tlm::tlm_dmi;
using namespace sc_core;
using namespace std::chrono;

static const unsigned int NUM_CACHED_TARGETS = 16;    // Router cache size is 16
static const unsigned int NUM_UNCACHED_TARGETS = 50;  // Set to a value less than 10 for evaluation
static const uint64_t TARGET_REGION_SIZE = 0x1000;    // 4KB per target region
static const unsigned int NUM_TRANSACTIONS = 1000000; // Number of transactions for benchmark

// A simple TLM target that just receives transactions and returns
class TestTarget : public sc_module
{
public:
    tlm_utils::simple_target_socket<TestTarget> target_socket;

    SC_CTOR (TestTarget) : target_socket("target_socket") {
        target_socket.register_b_transport(this, &TestTarget::b_transport);
    }

    void b_transport(tlm::tlm_generic_payload& trans, sc_time& delay)
    {
        // Do nothing, just return
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};

// A wrapper module to hold the router and its initiator socket
#include <tlm_utils/simple_initiator_socket.h> // Required for tlm_utils::simple_initiator_socket

class TopModule : public sc_module
{
public:
    router<32> router_inst;
    tlm_utils::simple_initiator_socket<TopModule> initiator_socket;

    TopModule(sc_module_name name): sc_module(name), router_inst("router_inst"), initiator_socket("initiator_socket")
    {
        initiator_socket.bind(router_inst.target_socket);
    }
};

class RouterCacheBench : public ::testing::Test
{
protected:
    std::unique_ptr<TopModule> top_module;
    std::vector<std::unique_ptr<TestTarget>> targets;

    void SetUp() override
    {
        sc_get_curr_simcontext()->reset(); // Reset SystemC kernel for a clean start
        top_module = std::make_unique<TopModule>("top_module");

        // Create and connect targets for both cached and uncached tests
        // Create enough targets for the cached test, as it requires more.
        for (unsigned int i = 0; i < std::max(NUM_CACHED_TARGETS, NUM_UNCACHED_TARGETS); ++i) {
            std::string name = "target_" + std::to_string(i);
            targets.push_back(std::make_unique<TestTarget>(name.c_str()));

            uint64_t target_base_addr = TARGET_REGION_SIZE * i;
            // Use add_target with a non-zero priority to avoid warnings
            top_module->router_inst.add_target(targets.back()->target_socket, target_base_addr, TARGET_REGION_SIZE,
                                               false, 100);
        }

        // This is crucial: Start SystemC simulation after all modules are set up
        std::cout << "[INFO] Calling sc_start(SC_ZERO_TIME) in SetUp for test "
                  << ::testing::UnitTest::GetInstance()->current_test_info()->name() << std::endl;
        sc_core::sc_start(sc_core::SC_ZERO_TIME);
        // This will trigger elaboration and start_of_simulation, ensuring all TLM callbacks are active.
        std::cout << "[INFO] sc_start(SC_ZERO_TIME) returned in SetUp." << std::endl;
    }

    void TearDown() override
    {
        std::cout << "[INFO] Calling sc_stop() in TearDown for test "
                  << ::testing::UnitTest::GetInstance()->current_test_info()->name() << std::endl;
        sc_core::sc_stop(); // Stop SystemC simulation for this test case
        std::cout << "[INFO] sc_stop() returned in TearDown." << std::endl;
        top_module.reset(); // Release unique_ptr, destruct TopModule and router
        targets.clear();    // Release unique_ptrs, destruct TestTargets
    }

    // Helper to perform a single TLM transaction
    void perform_transaction(uint64_t address)
    {
        tlm::tlm_generic_payload trans;
        sc_time delay = SC_ZERO_TIME;
        unsigned char data; // Dummy data
        trans.set_command(tlm::TLM_WRITE_COMMAND);
        trans.set_address(address);
        trans.set_data_ptr(&data);
        trans.set_data_length(1);
        // The following debug messages are kept for verbosity, but can be removed if desired
        // std::cout << "[DEBUG] Initiating transaction to address: 0x" << std::hex << address << std::endl;
        top_module->initiator_socket->b_transport(trans, delay);
        // std::cout << "[DEBUG] Transaction to address 0x" << std::hex << address << " completed. Delay: " << delay <<
        // std::endl;
        ASSERT_TRUE(trans.is_response_ok()); // Assert that the transaction was successful
    }

    // Benchmark function
    void run_benchmark(unsigned int num_targets_to_use, bool cached_test)
    {
        std::cout << "\nRunning benchmark for " << (cached_test ? "cached" : "uncached") << " accesses..." << std::endl;
        std::cout << "Using " << num_targets_to_use << " targets." << std::endl;
        std::cout << "Number of transactions: " << NUM_TRANSACTIONS << std::endl;

        // Warm-up phase
        for (unsigned int i = 0; i < num_targets_to_use; ++i) {
            perform_transaction(TARGET_REGION_SIZE * i);
        }

        // SystemC simulation is already started in SetUp().
        // No explicit sc_start() or sc_stop() calls within this function.

        std::cout << "[INFO] Warm-up complete. Starting benchmark for " << (cached_test ? "cached" : "uncached")
                  << " accesses..." << std::endl;
        auto start_time = high_resolution_clock::now();

        for (unsigned int i = 0; i < NUM_TRANSACTIONS; ++i) {
            uint64_t target_idx = i % num_targets_to_use;
            perform_transaction(TARGET_REGION_SIZE * target_idx);
        }

        auto end_time = high_resolution_clock::now();

        duration<double> time_span = duration_cast<duration<double>>(end_time - start_time);

        double transactions_per_second = NUM_TRANSACTIONS / time_span.count();
        std::cout << "Time taken: " << time_span.count() << " seconds" << std::endl;
        std::cout << "Transactions per second: " << static_cast<uint64_t>(transactions_per_second) << std::endl;
        ASSERT_GT(transactions_per_second, 0); // Ensure test runs and produces a result
    }
};

TEST_F(RouterCacheBench, CachedAccessPerformance) { run_benchmark(NUM_CACHED_TARGETS, true); }

TEST_F(RouterCacheBench, UncachedAccessPerformance) { run_benchmark(NUM_UNCACHED_TARGETS, false); }

int sc_main(int argc, char* argv[])
{
    // Initialize CCI broker
    // Initialize CCI broker for sc_main and Google Test environment
    cci_utils::consuming_broker broker("global_broker"); // Ensure a clean broker for each run
    cci_register_broker(broker);                         // Register the broker as the global one

    // Set up SystemC logging for the overall test run to show debug messages.
    scp::init_logging(scp::LogConfig().logLevel(scp::log::WARNING));

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
