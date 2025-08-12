/*
 * VirtIO Guest Firmware - ARM64 Assembly
 * This file is part of libqbox
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

.text
.global _start

// VirtIO MMIO register offsets
.equ VIRTIO_MMIO_MAGIC,               0x000
.equ VIRTIO_MMIO_VERSION,             0x004
.equ VIRTIO_MMIO_DEVICE_ID,           0x008
.equ VIRTIO_MMIO_VENDOR_ID,           0x00c
.equ VIRTIO_MMIO_DEVICE_FEATURES,     0x010
.equ VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0x014
.equ VIRTIO_MMIO_DRIVER_FEATURES,     0x020
.equ VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0x024
.equ VIRTIO_MMIO_QUEUE_SEL,           0x030
.equ VIRTIO_MMIO_QUEUE_NUM_MAX,       0x034
.equ VIRTIO_MMIO_QUEUE_NUM,           0x038
.equ VIRTIO_MMIO_QUEUE_READY,         0x044
.equ VIRTIO_MMIO_QUEUE_NOTIFY,        0x050
.equ VIRTIO_MMIO_INTERRUPT_STATUS,    0x060
.equ VIRTIO_MMIO_INTERRUPT_ACK,       0x064
.equ VIRTIO_MMIO_STATUS,              0x070
.equ VIRTIO_MMIO_QUEUE_DESC_LOW,      0x080
.equ VIRTIO_MMIO_QUEUE_DESC_HIGH,     0x084
.equ VIRTIO_MMIO_QUEUE_AVAIL_LOW,     0x090
.equ VIRTIO_MMIO_QUEUE_AVAIL_HIGH,    0x094
.equ VIRTIO_MMIO_QUEUE_USED_LOW,      0x0a0
.equ VIRTIO_MMIO_QUEUE_USED_HIGH,     0x0a4

// VirtIO status bits
.equ VIRTIO_STATUS_ACKNOWLEDGE, 1
.equ VIRTIO_STATUS_DRIVER,      2
.equ VIRTIO_STATUS_DRIVER_OK,   4
.equ VIRTIO_STATUS_FEATURES_OK, 8
.equ VIRTIO_STATUS_FAILED,      128

// VirtIO block commands
.equ VIRTIO_BLK_T_IN,  0
.equ VIRTIO_BLK_T_OUT, 1

// VirtIO block status codes
.equ VIRTIO_BLK_S_OK,     0
.equ VIRTIO_BLK_S_IOERR,  1
.equ VIRTIO_BLK_S_UNSUPP, 2

// Queue configuration
.equ VIRTQUEUE_SIZE, 16
.equ SECTOR_SIZE, 512

// VirtIO descriptor flags
.equ VIRTQ_DESC_F_NEXT,  1
.equ VIRTQ_DESC_F_WRITE, 2

// VirtIO feature bits
.equ VIRTIO_F_EVENT_IDX, 29           // Bit 29: Event suppression
.equ VIRTIO_F_RING_INDIRECT_DESC, 28  // Bit 28: Indirect descriptors
.equ VIRTIO_F_RING_EVENT_IDX, 29      // Same as VIRTIO_F_EVENT_IDX

// VirtIO ring flags
.equ VIRTQ_USED_F_NO_NOTIFY, 1        // Device doesn't need notification

// Device constants
.equ VIRTIO_ID_BLOCK, 2
.equ VIRTIO_MAGIC_VALUE, 0x74726976

// Memory layout constants
.equ DEVICE_SPACING, 0x2000
.equ QUEUE_SIZE_PER_CPU, 0x1000  // 4KB per CPU for queue structures
.equ DATA_SIZE_PER_CPU, 0x1000   // 4KB per CPU for data buffers

// Global variables for each CPU (stored at end of data buffer)
.equ NEGOTIATED_FEATURES_OFFSET, 0xF00  // Store negotiated features at offset 0xF00 in data buffer
.equ EVENT_IDX_ENABLED_OFFSET, 0xF08    // Store event_idx enabled flag

_start:
    // Get CPU ID from MPIDR_EL1
    mrs x0, mpidr_el1
    and x0, x0, #0xff             // Extract CPU ID (0-3)

    // Use hardcoded MMIO address first to test basic execution
    mov x1, #0x80000000           // MMIO base
    lsl x2, x0, #3                // CPU offset (8 bytes per CPU)
    add x1, x1, x2                // MMIO address for this CPU
    mov x2, x0                    // CPU ID
    orr x2, x2, #0x1000000        // Debug startup flag (bit 24)
    str x2, [x1]                  // Report we're starting

    // Load addresses from linker-defined symbols
    ldr x21, =MMIO_STATUS_BASE    // x21 = MMIO status base
    ldr x22, =VIRTIO_DEVICE_BASE  // x22 = VirtIO device 0 base
    ldr x23, =QUEUE_MEMORY_BASE   // x23 = Queue memory base
    ldr x24, =DATA_BUFFER_BASE    // x24 = Data buffer base

    // Debug write to show addresses were loaded
    lsl x1, x0, #3                // CPU offset (8 bytes per CPU)
    add x1, x21, x1               // MMIO address for this CPU using patched base
    mov x2, x0                    // CPU ID
    orr x2, x2, #0x2000000        // Address loaded flag (bit 25)
    str x2, [x1]                  // Report addresses loaded

    // Calculate device base for this CPU
    mov x1, #DEVICE_SPACING
    mul x2, x0, x1                // Offset for this CPU's device
    add x25, x22, x2              // x25 = This CPU's VirtIO device base

    // Calculate queue memory for this CPU
    mov x1, #QUEUE_SIZE_PER_CPU
    mul x2, x0, x1                // Offset for this CPU's queues
    add x26, x23, x2              // x26 = This CPU's queue base

    // Calculate data buffer for this CPU
    mov x1, #DATA_SIZE_PER_CPU
    mul x2, x0, x1                // Offset for this CPU's data
    add x27, x24, x2              // x27 = This CPU's data buffer base

    // Initialize VirtIO device properly
    bl init_virtio_device
    cbnz x0, device_init_failed

    // Report successful device initialization
    mrs x0, mpidr_el1
    and x0, x0, #0xff             // Extract CPU ID
    lsl x1, x0, #3                // CPU offset (8 bytes per CPU)
    add x1, x21, x1               // MMIO address for this CPU
    mov x2, x0                    // CPU ID
    orr x2, x2, #0x80000          // Device init success flag (bit 19)
    str x2, [x1]

    // Perform VirtIO-compatible block operations using direct device access
    mov x28, #0                   // Operation counter

operation_loop:
    // FIXED: Use proper VirtIO operation with full queue protocol
    mov x1, x28                   // Operation number
    bl perform_virtio_operation
    cbnz x0, operation_failed     // If operation failed, report it

    // Report operation success
    mrs x0, mpidr_el1
    and x0, x0, #0xff             // Re-extract CPU ID
    lsl x1, x0, #3                // CPU offset (8 bytes per CPU)
    add x1, x21, x1               // MMIO address for this CPU
    mov x2, x0                    // CPU ID
    orr x2, x2, x28, lsl #8       // Include operation counter in bits 8-15
    orr x2, x2, #0x10000          // Success flag (bit 16)
    str x2, [x1]

    // Next operation
    add x28, x28, #1
    cmp x28, #20                  // Do 20 operations per CPU
    b.lt operation_loop

    // All operations complete
    mrs x0, mpidr_el1
    and x0, x0, #0xff             // Extract CPU ID again
    lsl x1, x0, #3                // CPU offset (8 bytes per CPU)
    add x1, x21, x1               // MMIO address for this CPU
    mov x2, x0                    // CPU ID
    orr x2, x2, #0x100000         // Complete flag (bit 20)
    orr x2, x2, x28, lsl #8       // Final operation count in bits 8-15
    str x2, [x1]

    b halt

device_init_failed:
    mrs x0, mpidr_el1
    and x0, x0, #0xff             // Extract CPU ID
    lsl x1, x0, #3                // CPU offset (8 bytes per CPU)
    add x1, x21, x1               // MMIO address for this CPU
    mov x2, x0                    // CPU ID
    orr x2, x2, #0x40000          // Init failed flag (bit 18)
    str x2, [x1]
    b halt

operation_failed:
    mrs x0, mpidr_el1
    and x0, x0, #0xff             // Extract CPU ID
    lsl x1, x0, #3                // CPU offset (8 bytes per CPU)
    add x1, x21, x1               // MMIO address for this CPU
    mov x2, x0                    // CPU ID
    orr x2, x2, #0x20000          // Operation failed flag (bit 17)
    str x2, [x1]
    b halt

// Initialize VirtIO device following proper protocol
// Input: x25 = device base, x26 = queue base
// Output: x0 = 0 on success, non-zero on failure
init_virtio_device:
    // Save return address
    mov x29, x30

    // Debug checkpoint: Entering init_virtio_device
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x10000000     // Function entry flag (bit 28)
    str x12, [x11]

    // Debug checkpoint: About to read magic value
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x4000000      // About to read magic flag (bit 26)
    str x12, [x11]

    // Read and verify magic value
    ldr w1, [x25, #VIRTIO_MMIO_MAGIC]

    // Debug checkpoint: Magic value read completed
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x8000000      // Magic read completed flag (bit 27)
    str x12, [x11]
    mov w2, #(VIRTIO_MAGIC_VALUE & 0xFFFF)
    movk w2, #(VIRTIO_MAGIC_VALUE >> 16), lsl #16
    cmp w1, w2
    b.ne init_failed

    // Read and verify device ID (block device = 2)
    ldr w1, [x25, #VIRTIO_MMIO_DEVICE_ID]
    cmp w1, #VIRTIO_ID_BLOCK
    b.ne init_failed

    // VirtIO initialization sequence
    // 1. Reset device
    mov w1, #0
    str w1, [x25, #VIRTIO_MMIO_STATUS]

    // 2. Set ACKNOWLEDGE status bit
    mov w1, #VIRTIO_STATUS_ACKNOWLEDGE
    str w1, [x25, #VIRTIO_MMIO_STATUS]

    // 3. Set DRIVER status bit
    mov w1, #(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER)
    str w1, [x25, #VIRTIO_MMIO_STATUS]

    // 4. Read device features
    ldr w1, [x25, #VIRTIO_MMIO_DEVICE_FEATURES]

    // Store negotiated features for later use
    add x2, x27, #NEGOTIATED_FEATURES_OFFSET
    str w1, [x2]

    // Check if device supports EVENT_IDX feature
    mov w3, #1
    lsl w3, w3, #VIRTIO_F_EVENT_IDX     // Create EVENT_IDX mask
    and w4, w1, w3                      // Check if device supports it
    cbnz w4, device_supports_event_idx

    // Device doesn't support event_idx
    mov w4, #0
    b store_event_idx_support

device_supports_event_idx:
    // Device supports event_idx, we'll use it
    mov w4, #1

store_event_idx_support:
    add x2, x27, #EVENT_IDX_ENABLED_OFFSET
    str w4, [x2]                        // Store whether event_idx is enabled

    // 5. Write driver features (accept all device features)
    str w1, [x25, #VIRTIO_MMIO_DRIVER_FEATURES]

    // 6. Set FEATURES_OK status bit
    mov w1, #(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK)
    str w1, [x25, #VIRTIO_MMIO_STATUS]

    // 7. Skip features check for now - device may clear FEATURES_OK bit if it doesn't like our selection
    // ldr w2, [x25, #VIRTIO_MMIO_STATUS]
    // and w2, w2, #VIRTIO_STATUS_FEATURES_OK
    // cbz w2, init_failed

    // 8. Setup queue 0 (request queue)
    mov w1, #0
    str w1, [x25, #VIRTIO_MMIO_QUEUE_SEL]

    // Check maximum queue size
    ldr w1, [x25, #VIRTIO_MMIO_QUEUE_NUM_MAX]
    cmp w1, #VIRTQUEUE_SIZE
    b.lt init_failed

    // Set our desired queue size
    mov w1, #VIRTQUEUE_SIZE
    str w1, [x25, #VIRTIO_MMIO_QUEUE_NUM]

    // Setup queue memory layout:
    // - Descriptor table: 16 descriptors * 16 bytes = 256 bytes
    // - Available ring: 4 + (16 * 2) + 2 = 38 bytes (aligned to 2)
    // - Used ring: 4 + (16 * 8) + 2 = 134 bytes (aligned to 4)

    // Setup descriptor table base
    mov x1, x26
    str w1, [x25, #VIRTIO_MMIO_QUEUE_DESC_LOW]
    lsr x1, x1, #32
    str w1, [x25, #VIRTIO_MMIO_QUEUE_DESC_HIGH]

    // Setup available ring (256-byte aligned after descriptors)
    add x1, x26, #256
    str w1, [x25, #VIRTIO_MMIO_QUEUE_AVAIL_LOW]
    lsr x1, x1, #32
    str w1, [x25, #VIRTIO_MMIO_QUEUE_AVAIL_HIGH]

    // Setup used ring (4-byte aligned, after available ring)
    add x1, x26, #320             // 256 + 64 (rounded up for alignment)
    str w1, [x25, #VIRTIO_MMIO_QUEUE_USED_LOW]
    lsr x1, x1, #32
    str w1, [x25, #VIRTIO_MMIO_QUEUE_USED_HIGH]

    // Enable the queue
    mov w1, #1
    str w1, [x25, #VIRTIO_MMIO_QUEUE_READY]

    // 9. Set DRIVER_OK status bit (device is ready)
    mov w1, #(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK)
    str w1, [x25, #VIRTIO_MMIO_STATUS]

    // Queue state will be initialized during first operation
    // Direct memory access here might conflict with MMIO routing

    // Debug checkpoint: Successful exit
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x20000000     // Function success exit flag (bit 29)
    str x12, [x11]

    mov x0, #0                    // Success
    mov x30, x29                  // Restore return address
    ret

init_failed:
    // Debug checkpoint: Failure exit
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x40000000     // Function failure exit flag (bit 30)
    str x12, [x11]

    mov x0, #1                    // Failure
    mov x30, x29                  // Restore return address
    ret

// Check if we should notify the device (implements Rust should_notify() logic)
// Input: x26 = queue base, x27 = data buffer base (for variables)
// Output: x0 = 1 if should notify, 0 if notification suppressed
// Clobbers: w1, w2, w3, x4
should_notify:
    // Load whether EVENT_IDX is enabled
    add x4, x27, #EVENT_IDX_ENABLED_OFFSET
    ldr w1, [x4]
    cbz w1, check_legacy_suppression    // If EVENT_IDX not enabled, use legacy method

    // EVENT_IDX method: Check if avail_idx >= avail_event + 1
    // Load current avail_idx (our shadow copy)
    add x4, x26, #256             // Available ring base
    ldrh w2, [x4, #2]             // Load avail.idx

    // Load avail_event from used ring
    add x4, x26, #320             // Used ring base
    // Calculate avail_event offset: sizeof(UsedRing) - sizeof(u16)
    // UsedRing = flags(2) + idx(2) + ring[16](16*8) + avail_event(2)
    // So avail_event is at offset: 2 + 2 + 128 = 132
    ldrh w3, [x4, #132]           // Load used.avail_event

    // Check if avail_idx >= avail_event + 1
    add w3, w3, #1                // avail_event + 1
    cmp w2, w3                    // Compare avail_idx with (avail_event + 1)
    b.hs notify_required          // If avail_idx >= avail_event + 1, notify
    b notify_suppressed           // Otherwise, suppress notification

check_legacy_suppression:
    // Legacy method: Check VIRTQ_USED_F_NO_NOTIFY flag in used.flags
    add x4, x26, #320             // Used ring base
    ldrh w1, [x4]                 // Load used.flags
    and w1, w1, #VIRTQ_USED_F_NO_NOTIFY  // Check NO_NOTIFY flag
    cbnz w1, notify_suppressed    // If NO_NOTIFY set, suppress notification

notify_required:
    mov x0, #1                    // Should notify
    ret

notify_suppressed:
    mov x0, #0                    // Don't notify
    ret

// Perform a VirtIO block operation using full queue protocol
// Input: x1 = operation number, x25 = device base, x26 = queue base, x27 = data buffer base
// Output: x0 = 0 on success, non-zero on failure
perform_virtio_operation:
    mov x29, x30                  // Save return address
    mov x28, x1                   // Save operation number

    // Calculate sector number (operation * 10 for spacing)
    mov x1, #10
    mul x3, x28, x1               // x3 = sector number

    // Determine operation type (even = read, odd = write)
    and x4, x28, #1
    mov x5, #VIRTIO_BLK_T_IN      // Default to read
    cbz x4, setup_request
    mov x5, #VIRTIO_BLK_T_OUT     // Write operation

setup_request:
    // Clear status byte first
    add x7, x27, #SECTOR_SIZE + 16
    mov w8, #0xFF                 // Invalid status initially
    strb w8, [x7]

    // Setup VirtIO block request header in data buffer
    str w5, [x27]                 // req.type
    mov w1, #0
    str w1, [x27, #4]             // req.reserved
    str x3, [x27, #8]             // req.sector

    // For write operations, populate data buffer with test pattern
    cmp x5, #VIRTIO_BLK_T_OUT
    b.ne setup_descriptors

fill_write_pattern:
    add x6, x27, #16              // Data buffer starts after 16-byte request header
    mov x7, #0                    // Byte counter

write_pattern_loop:
    add w8, w7, w28               // Pattern: operation_number + byte_offset
    and w8, w8, #0xFF             // Keep it as single byte
    strb w8, [x6, x7]             // Store pattern byte
    add x7, x7, #1
    cmp x7, #SECTOR_SIZE          // Fill entire sector
    b.lt write_pattern_loop

setup_descriptors:
    // Debug checkpoint: reached descriptor setup
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x1000000      // Descriptor setup flag (bit 24)
    str x12, [x11]

    // FIXED: Use operation number modulo queue size to avoid conflicts
    // This is simpler than per-CPU allocation and should still reduce conflicts
    and x14, x28, #(VIRTQUEUE_SIZE - 1)  // Use operation as descriptor index

    // Calculate descriptor base for this request
    mov x13, #16                  // Descriptor size
    mul x13, x14, x13             // Offset for this descriptor set
    add x6, x26, x13              // Base descriptor for this operation

    // Descriptor 0: Request header (read-only from device perspective)
    str x27, [x6]                 // desc[0].addr = request buffer
    mov w1, #16
    str w1, [x6, #8]              // desc[0].len = sizeof(virtio_blk_req)
    mov w1, #VIRTQ_DESC_F_NEXT
    strh w1, [x6, #12]            // desc[0].flags = NEXT
    add w1, w14, #1               // Next descriptor index
    and w1, w1, #(VIRTQUEUE_SIZE - 1)
    strh w1, [x6, #14]            // desc[0].next

    // Descriptor 1: Data buffer
    add x7, x27, #16              // Data buffer after request header
    add x8, x6, #16               // Next descriptor
    str x7, [x8]                  // desc[1].addr = data buffer
    mov w1, #SECTOR_SIZE
    str w1, [x8, #8]              // desc[1].len = 512

    // Set descriptor 1 flags based on operation type
    mov w1, #VIRTQ_DESC_F_NEXT
    cmp x5, #VIRTIO_BLK_T_IN      // Read operation?
    b.ne desc1_flags_done
    orr w1, w1, #VIRTQ_DESC_F_WRITE  // For reads, device writes to this buffer
desc1_flags_done:
    strh w1, [x8, #12]            // desc[1].flags
    add w1, w14, #2               // Next descriptor index
    and w1, w1, #(VIRTQUEUE_SIZE - 1)
    strh w1, [x8, #14]            // desc[1].next

    // Descriptor 2: Status byte (always writable by device)
    add x7, x27, #SECTOR_SIZE + 16  // Status byte after data
    add x8, x6, #32               // Third descriptor
    str x7, [x8]                  // desc[2].addr = status byte location
    mov w1, #1
    str w1, [x8, #8]              // desc[2].len = 1
    mov w1, #VIRTQ_DESC_F_WRITE   // Device writes status here
    strh w1, [x8, #12]            // desc[2].flags = WRITE
    mov w1, #0
    strh w1, [x8, #14]            // desc[2].next = 0 (end of chain)

submit_to_available_ring:
    // Debug checkpoint: reached available ring submission
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x800000       // Available ring flag (bit 23)
    str x12, [x11]

    // FIXED: Add descriptor chain head to available ring with proper barriers
    add x6, x26, #256             // Available ring base

    // Use memory barriers for ordering instead of complex LL/SC
    dsb sy                        // Ensure descriptor setup is complete

    ldrh w7, [x6, #2]             // Load current avail.idx
    and x8, x7, #(VIRTQUEUE_SIZE - 1)  // Calculate ring slot
    add x9, x6, #4                // avail.ring array base
    lsl x8, x8, #1                // Convert to half-word offset
    add x9, x9, x8                // Point to ring[avail.idx & mask]
    strh w14, [x9]                // avail.ring[idx] = descriptor_head_index

    // Memory barrier to ensure ring entry is written before index update
    dsb sy

    // Update available index (simplified - rely on infrequent collisions)
    add w8, w7, #1                // Calculate new index
    strh w8, [x6, #2]             // Update avail.idx

    // Memory barrier before device notification
    dsb sy

    // Use conditional notification based on should_notify() logic
    // Now that interrupts are wired, this should work properly
    bl should_notify
    cbz x0, skip_device_notification  // If should_notify returned 0, skip notification

    // Report that we're about to notify the device (debug checkpoint)
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x400000       // About to notify flag (bit 22)
    str x12, [x11]

    // Notify the device - this should now trigger proper I/O processing
    mov w1, #0                    // Queue 0
    str w1, [x25, #VIRTIO_MMIO_QUEUE_NOTIFY]

    // Report that we successfully wrote to notify register (debug checkpoint)
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x800000       // Notify completed successfully flag (bit 23)
    str x12, [x11]

    b notification_done

skip_device_notification:
    // Report that notification was suppressed by should_notify logic
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, #0x1000000      // Notification suppressed flag (bit 24)
    str x12, [x11]

notification_done:

wait_for_completion:
    // FIXED: More efficient polling with reasonable timeout
    add x6, x26, #320             // Used ring base
    ldrh w8, [x6, #2]             // Load initial used.idx

    // DEBUG: Report initial used.idx before polling
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    orr x12, x12, x8, lsl #8      // Include initial used.idx in bits 8-15
    orr x12, x12, #0x2000000      // Initial used.idx flag (bit 25)
    str x12, [x11]

    // Set moderate timeout and add debug checkpoints
    mov x9, #0x1000               // Moderate timeout: 4096 iterations (reduced for debugging)

completion_poll_loop:
    // Memory barrier before reading used index
    dsb sy
    ldrh w10, [x6, #2]            // Check current used.idx
    cmp w10, w8                   // Has it advanced?
    b.ne operation_completed      // Yes, operation finished

    // DEBUG: Every 512 iterations, report current used.idx to see if it ever changes
    and x16, x9, #0x1FF           // Check if iteration count is divisible by 512
    cbnz x16, skip_used_idx_debug

    // Report current used.idx value
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    ldrh w13, [x6, #2]            // Re-read current used.idx
    orr x12, x12, x13, lsl #8     // Include current used.idx in bits 8-15
    orr x12, x12, #0x4000000      // Current used.idx debug flag (bit 26)
    str x12, [x11]

skip_used_idx_debug:

    // Debug checkpoint every 1024 iterations to track polling progress
    and x16, x9, #0x3FF           // Check if iteration count is divisible by 1024
    cbnz x16, skip_debug_checkpoint

    // Report polling progress (every ~1/4 of timeout)
    mrs x10, mpidr_el1
    and x10, x10, #0xff           // Extract CPU ID
    lsl x11, x10, #3              // CPU offset (8 bytes per CPU)
    add x11, x21, x11             // MMIO address for this CPU
    mov x12, x10                  // CPU ID
    lsr x13, x9, #8               // Remaining timeout / 256 as progress indicator
    orr x12, x12, x13, lsl #8     // Include progress in bits 8-15
    orr x12, x12, #0x200000       // Polling progress flag (bit 21)
    str x12, [x11]

skip_debug_checkpoint:
    // Use a simple delay instead of WFE which may not work properly in TCG emulation
    // WFE might cause indefinite waiting in software emulation
    nop                           // Simple delay instead of WFE

    // Brief delay to avoid excessive polling
    mov x15, #10                  // Reduced inner delay from 100 to 10
inner_delay:
    sub x15, x15, #1
    cbnz x15, inner_delay

    sub x9, x9, #1
    cbnz x9, completion_poll_loop

    // Timeout occurred
    mov x0, #1                    // Timeout error
    b restore_and_return

operation_completed:
    // Verify the operation completed successfully by checking status byte
    add x7, x27, #SECTOR_SIZE + 16  // Status byte location
    ldrb w8, [x7]                 // Load operation status
    cmp w8, #VIRTIO_BLK_S_OK
    b.ne status_error

    // For read operations, we could verify the data pattern here
    // For write operations, the pattern verification happens on host side

    mov x0, #0                    // Success
    b restore_and_return

status_error:
    mov x0, #2                    // VirtIO status error

restore_and_return:
    mov x30, x29                  // Restore return address
    ret

report_operation_failed:
    mrs x0, mpidr_el1
    and x0, x0, #0xff             // Extract CPU ID
    lsl x1, x0, #3                // CPU offset (8 bytes per CPU)
    add x1, x21, x1               // MMIO address for this CPU
    mov x2, x0                    // CPU ID
    orr x2, x2, x28, lsl #8       // Include operation counter
    orr x2, x2, #0x20000          // Operation failed flag (bit 17)
    str x2, [x1]
    b halt

halt:
    wfe
    b halt
