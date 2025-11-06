/*
 * Hexagon SMMU Stress Test Firmware
 * Translated from ARM64 version in smmu_router_stress_test_v2.cc
 *
 * This firmware is designed to run on Hexagon CPUs behind SMMU TBUs,
 * testing address translation and multi-CPU memory access patterns.
 *
 * Memory Layout:
 *   0x00000000: Boot loader (jumps to main firmware)
 *   0x00000200: Diagnostic error handler
 *   0x00001000: Main firmware (test logic)
 *
 * Register Usage:
 *   r16: CPU ID (persistent)
 *   r17: Tester base address (persistent)
 *   r18: Iteration counter (persistent)
 *   r19: Region ID (persistent)
 *   r20: Page number (persistent)
 *   r21: Number of pages per region (persistent)
 *   r0-r15: Temporary/scratch registers
 *   r31: Link register (return address)
 */

    .text
    .align 4

/*
 * ============================================================================
 * BOOT LOADER at 0x00000000
 * ============================================================================
 * Simple boot loader that jumps to main firmware at 0x1000
 */
    .org 0x0
    .globl boot_start
boot_start:
    // Jump to main firmware at 0x1000
    r0 = ##0x1000           // Load main firmware address (extended immediate)
    jumpr r0                // Jump to main firmware

/*
 * ============================================================================
 * DIAGNOSTIC ERROR HANDLER at 0x00000200
 * ============================================================================
 * Called when a fatal error occurs. Reports error to tester and halts.
 */
    .org 0x200
    .globl diagnostic_error
diagnostic_error:
    // Get CPU ID (assuming it's stored in a known location or passed)
    // For Hexagon multi-threading, thread ID might be available via special register
    // This is a placeholder - actual implementation depends on system setup
    r16 = htid
    
    // Calculate tester base address for this CPU
    r17 = ##0x40000         // TESTER_ADDR base
    r1 = #0x100             // Register size per CPU
    r1 = mpyi(r1, r16)      // CPU offset = 0x100 * CPU_ID
    r17 = add(r17, r1)      // r17 = tester base for this CPU
    
    // Send error message: 0xE000 + CPU_ID
    r2 = #0xE000            // Error code base
    r2 = add(r2, r16)       // Add CPU ID
    memw(r17 + #0x28) = r2  // Write to REG_DEBUG (offset 0x28)
    
    // Halt execution
    // Note: Hexagon wait instruction encoding may vary by version
    // Using a loop as fallback
diagnostic_loop:
    jump diagnostic_loop    // Infinite loop (halt)

/*
 * ============================================================================
 * MAIN FIRMWARE at 0x00001000
 * ============================================================================
 * Main test logic: coordinates with tester to fill/check memory regions
 */
    .org 0x1000
    .globl main_start
main_start:
    // Get CPU ID - This is architecture-specific
    // For Hexagon, we might need to read from a system register or memory location
    // Placeholder: assume CPU ID is passed or can be retrieved
    // In a real system, this might come from hexagon_globalreg or thread ID
    r16 = htid
    
    // Calculate tester base address for this CPU
    r17 = ##0x40000         // TESTER_ADDR = 0x40000
    r1 = #0x100             // Register size per CPU
    r1 = mpyi(r1, r16)      // CPU offset
    r17 = add(r17, r1)      // r17 = CPU-specific tester base
    
    // Signal startup to tester
    r0 = #0x1000            // Startup debug message
    memw(r17 + #0x28) = r0  // Write to REG_DEBUG
    
    // Initialize iteration counter
    r18 = #0                // r18 = iteration counter
    
    // Load max iterations (placeholder - would be configured)
    r2 = ##0x7fff           // MAX_ITERATIONS (32767)

/*
 * ============================================================================
 * MAIN LOOP
 * ============================================================================
 * Repeatedly request regions to fill, fill them, and report completion
 */
main_loop:
    // Check if we've reached max iterations
    p0 = cmp.gt(r18, r2)    // Compare iteration counter with max
    if (p0) jump test_complete
    
    // Signal entering main loop
    r0 = #0x2000            // Main loop debug message
    r0 = add(r0, r18)       // Include iteration count
    memw(r17 + #0x28) = r0  // Write to REG_DEBUG
    
    // Request a region to fill
    r0 = #1                 // FILL_REQUEST = 1
    memw(r17 + #0x00) = r0  // Write to REG_REQUEST

/*
 * Poll for readiness - wait for tester to assign a region
 */
poll_fill:
    r0 = #0x3000            // Polling debug message
    memw(r17 + #0x28) = r0  // Write to REG_DEBUG
    
    r0 = memw(r17 + #0x08)  // Read REG_STATUS
    p0 = cmp.eq(r0, #1)     // Check if READY (status == 1)
    if (p0) jump fill_ready
    
    p0 = cmp.eq(r0, #0)     // Check if BUSY (status == 0)
    if (p0) jump try_check  // Try checking instead
    
    jump poll_fill          // Keep polling

/*
 * Region is ready - fill it with pattern
 */
fill_ready:
    // Get assigned region ID
    r19 = memw(r17 + #0x10) // Read REG_REGION_ID
    
    // Signal starting work
    r0 = #0x4000            // Starting work debug message
    r0 = add(r0, r19)       // Include region ID
    memw(r17 + #0x28) = r0  // Write to REG_DEBUG
    
    // Set up for filling the region
    // Virtual address base (high address that SMMU will translate)
    r3:2 = ##0x300000000    // VIRTUAL_TEST_ADDR (64-bit, 12GB)
    
    // Boundary bytes to write (80 bytes = 10 words)
    r4 = #80                // BOUNDARY_BYTES
    r4 = lsr(r4, #3)        // Convert to 8-byte words (divide by 8)
    
    // Initialize page loop
    r20 = #0                // Page number (starts at 0)
    r21 = #2                // Number of pages per region (8KB / 4KB = 2)

/*
 * Loop over pages in the region
 */
fill_page_loop:
    p0 = cmp.gtu(r20, r21)  // Compare page_num with num_pages
    if (p0) jump fill_done
    
    // Calculate page base address: base + (page_num * PAGE_SIZE)
    r8 = #0x1000            // PAGE_SIZE = 4KB = 0x1000
    r8 = mpyi(r8, r20)      // page_offset = page_num * PAGE_SIZE
    
    // Add offset to base address (64-bit addition)
    r9:8 = combine(r3, r2)  // Copy base address to r9:8
    r11:10 = combine(#0, r8) // Convert 32-bit offset to 64-bit
    r9:8 = add(r9:8, r11:10) // Add page offset (64-bit + 64-bit)
    
    // Call fill_boundary for start of page
    // Arguments: r1:0 = base address
    r1:0 = combine(r9, r8)
    call fill_boundary
    
    // Calculate end boundary address
    r6 = #0x1000            // PAGE_SIZE
    r7 = asl(r4, #3)        // words * 8 (convert back to bytes)
    r6 = sub(r6, r7)        // offset = PAGE_SIZE - (words * 8)
    
    // Add offset to page base
    r1:0 = combine(r9, r8)  // Restore page base
    r11:10 = combine(#0, r6) // Convert 32-bit offset to 64-bit
    r1:0 = add(r1:0, r11:10) // Add end offset (64-bit + 64-bit)
    
    // Call fill_boundary for end of page
    call fill_boundary
    
    // Next page
    r20 = add(r20, #1)
    jump fill_page_loop

/*
 * Filling complete - notify tester
 */
fill_done:
    r0 = #1                 // FILL_DONE = 1
    memw(r17 + #0x18) = r0  // Write to REG_COMPLETE
    
    // Increment iteration counter
    r18 = add(r18, #1)
    jump main_loop

/*
 * No regions available to fill - try checking
 */
try_check:
    // For now, just loop back (checking not implemented in this version)
    jump main_loop

/*
 * Test complete - all iterations done
 */
test_complete:
    jump end

/*
 * End of test - infinite loop
 */
end:
    wait(r0)
    jump end                // Infinite loop

/*
 * ============================================================================
 * FILL_BOUNDARY FUNCTION
 * ============================================================================
 * Fills a memory boundary with a verifiable pattern.
 * 
 * Pattern format (64-bit):
 *   Bits 63:32 - CPU ID
 *   Bits 31:16 - Region ID
 *   Bits 15:8  - Page number
 *   Bits 7:0   - Word offset
 *
 * Arguments:
 *   r1:0 - Base address to start writing (64-bit)
 *
 * Uses:
 *   r16 - CPU ID (global)
 *   r19 - Region ID (global)
 *   r20 - Page number (global)
 *   r4  - Number of words to write (global)
 *   r5  - Word offset (loop counter)
 *   r6  - Pattern value (64-bit, uses r7:6)
 *   r8  - Temporary for address calculation
 *
 * Clobbers: r5, r6, r7, r8, r9
 */
    .globl fill_boundary
fill_boundary:
    // Save return address (r31 is link register)
    // Hexagon automatically saves r31 on call
    
    // Initialize loop counter
    r5 = #0                 // Word offset = 0

fill_boundary_loop:
    // Check if done
    p0 = cmp.gtu(r5, r4)    // Compare offset with num_words
    if (p0) jump fill_boundary_done
    
    // Build pattern value (64-bit)
    // Pattern = (CPU_ID << 32) | (REGION_ID << 16) | (PAGE_NUM << 8) | WORD_OFFSET
    
    // Start with CPU ID in upper 32 bits
    r7 = r16                // CPU ID
    r6 = #0                 // Clear lower 32 bits
    
    // Add Region ID (bits 31:16)
    r8 = asl(r19, #16)      // Shift region ID to bits 31:16
    r6 = or(r6, r8)         // OR into lower 32 bits
    
    // Add Page number (bits 15:8)
    r8 = asl(r20, #8)       // Shift page number to bits 15:8
    r6 = or(r6, r8)         // OR into lower 32 bits
    
    // Add Word offset (bits 7:0)
    r6 = or(r6, r5)         // OR word offset into bits 7:0
    
    // Now r7:6 contains the full 64-bit pattern
    
    // Calculate write address: base_addr + (word_offset * 8)
    r8 = asl(r5, #3)        // word_offset * 8 (bytes)
    r9:8 = combine(r1, r0)  // Copy base address
    r11:10 = combine(#0, r8) // Convert 32-bit offset to 64-bit
    r9:8 = add(r9:8, r11:10) // Add offset (64-bit + 64-bit)
    
    // Write pattern to memory (64-bit store)
    memd(r8) = r7:6         // Store double-word
    
    // Increment word offset
    r5 = add(r5, #1)
    jump fill_boundary_loop

fill_boundary_done:
    // Return to caller
    jumpr r31               // Return (jump to link register)

/*
 * ============================================================================
 * END OF FIRMWARE
 * ============================================================================
 */
