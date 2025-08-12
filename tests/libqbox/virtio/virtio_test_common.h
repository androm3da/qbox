/*
 * This file is part of libqbox
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>

/*
 * Common VirtIO test definitions and constants
 * Shared across VirtIO MMIO block, network, and sound tests
 */

namespace VirtIOTestCommon {
// Common VirtIO test configuration
static constexpr uint64_t VIRTIO_BASE = 0x10000000;
static constexpr uint64_t VIRTIO_SIZE = 0x1000;
static constexpr int NUM_ITERATIONS = 5; // Number of reset iterations

// VirtIO magic and version constants
static constexpr uint32_t VIRTIO_MAGIC_VALUE = 0x74726976;
static constexpr uint32_t VIRTIO_VERSION = 2;

// VirtIO device IDs (from virtio_ids.h)
static constexpr uint32_t VIRTIO_ID_NET = 1;
static constexpr uint32_t VIRTIO_ID_BLOCK = 2;
static constexpr uint32_t VIRTIO_ID_SOUND = 25;

// VirtIO MMIO register offsets
static constexpr uint64_t VIRTIO_MMIO_MAGIC = 0x000;
static constexpr uint64_t VIRTIO_MMIO_VERSION = 0x004;
static constexpr uint64_t VIRTIO_MMIO_DEVICE_ID = 0x008;
static constexpr uint64_t VIRTIO_MMIO_VENDOR_ID = 0x00c;
static constexpr uint64_t VIRTIO_MMIO_DEVICE_FEATURES = 0x010;
static constexpr uint64_t VIRTIO_MMIO_DEVICE_FEATURES_SEL = 0x014;
static constexpr uint64_t VIRTIO_MMIO_DRIVER_FEATURES = 0x020;
static constexpr uint64_t VIRTIO_MMIO_DRIVER_FEATURES_SEL = 0x024;
static constexpr uint64_t VIRTIO_MMIO_STATUS = 0x070;

// VirtIO status bits
static constexpr uint32_t VIRTIO_STATUS_ACKNOWLEDGE = 1;
static constexpr uint32_t VIRTIO_STATUS_DRIVER = 2;
static constexpr uint32_t VIRTIO_STATUS_DRIVER_OK = 4;
static constexpr uint32_t VIRTIO_STATUS_FEATURES_OK = 8;
static constexpr uint32_t VIRTIO_STATUS_FAILED = 128;
} // namespace VirtIOTestCommon
