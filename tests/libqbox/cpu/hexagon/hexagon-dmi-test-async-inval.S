.text
.global _start

_start:
    // Load constants from the end of the binary (patched by test framework)
    r0 = ##dmi_addr_const        // Load address of DMI constant
    r1 = memw(r0)                // r1 = patched DMI base address (s0 in RISC-V)

    r0 = ##mmio_addr_const       // Load address of MMIO constant
    r2 = memw(r0)                // r2 = patched MMIO base address (s1 in RISC-V)

    r0 = ##num_writes_const      // Load address of num_writes constant
    r3 = memw(r0)                // r3 = patched num_writes value (s2 in RISC-V)

    // Get CPU ID (simplified - use 0 for single CPU)
    r4 = #0                      // r4 = CPU ID (t1 in RISC-V)

    // Calculate CPU-specific offset (multiply by 8)
    r5 = asl(r4, #3)             // r5 = CPU ID * 8 (t2 in RISC-V)

    // Add offset to base addresses
    r1 = add(r1, r5)             // r1 = DMI address for this CPU
    r2 = add(r2, r5)             // r2 = MMIO address for this CPU

    // Init: will ask for a DMI pointer and go from ST_START to ST_READ_DMI
    r6 = memw(r1)                // Read from DMI (t3 in RISC-V)
    memw(r2) = r6                // Write to MMIO (report)

    // Initialize counter
    r7 = #0                      // r7 = counter (t4 in RISC-V)

loop:
    // Read on DMI socket, report the read on the control socket
    r6 = memw(r1)                // Read from DMI
    memw(r2) = r6                // Write to MMIO (report)

    // Increment counter
    r7 = add(r7, #1)             // r7++

    // Write on DMI socket, report the write on the control socket
    memw(r1) = r7                // Write counter to DMI
    memw(r2) = r7                // Write to MMIO (report)

    // Check if we've done enough writes
    p0 = cmp.eq(r7, r3)          // if (r7 == num_writes)
    if (p0) jump end             // goto end
    jump loop                    // else goto loop

end:
    // Use wait instruction - this word encoding is from existing hexagon tests
    .word 0x6440c000
    jump end

// Constants section - will be filled in by the test at runtime
.align 4
dmi_addr_const:
    .word 0x90000000
mmio_addr_const:
    .word 0x80000000
num_writes_const:
    .word 0x00000200
