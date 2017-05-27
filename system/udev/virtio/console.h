// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include "device.h"
#include "ring.h"

#include <magenta/compiler.h>
#include <mxtl/unique_ptr.h>
#include <stdlib.h>

namespace virtio {

class Ring;

class ConsoleDevice : public Device {
public:
    ConsoleDevice(mx_driver_t* driver, mx_device_t* device);
    virtual ~ConsoleDevice();

    virtual mx_status_t Init();

    virtual void IrqRingUpdate();
    virtual void IrqConfigChange();

private:
    static mx_status_t virtio_console_read(void* ctx, void* buf, size_t count, mx_off_t off, size_t* actual);
    static mx_status_t virtio_console_write(void* ctx, const void* buf, size_t count, mx_off_t off, size_t* actual);

    void HandleControlMessage(size_t);

    mx_status_t virtio_console_start();
    static int virtio_console_start_entry(void* arg);
    thrd_t start_thread_ = {};

    // request condition
    mxtl::Mutex request_lock_;
    cnd_t request_cond_ = {};

    // control tx/rx rings
    Ring control_rx_vring_ = {this};
    Ring control_tx_vring_ = {this};

    // port rings
    mxtl::unique_ptr<Ring> port_rx_ring_[32];
    mxtl::unique_ptr<Ring> port_tx_ring_[32];
    mx_device_t *port0_device_ = nullptr;
    mx_protocol_device_t port0_device_ops_ = {};

    // saved block device configuration out of the pci config BAR
    struct virtio_console_config {
        uint16_t cols;
        uint16_t rows;
        uint32_t max_ports;
        uint32_t emerg_wr;
    } config_ __PACKED = {};

    // a large transfer buffer
    uint8_t* transfer_buffer_ = nullptr;
    mx_paddr_t transfer_buffer_pa_ = 0;

    uint8_t* control_rx_buffer_ = nullptr;
    mx_paddr_t control_rx_buffer_pa_ = 0;

    uint8_t* control_tx_buffer_ = nullptr;
    mx_paddr_t control_tx_buffer_pa_ = 0;

    static const size_t port_buffer_size = 512;
    uint8_t* port0_rx_buffer_ = nullptr;
    mx_paddr_t port0_rx_buffer_pa_ = 0;

    uint8_t* port0_tx_buffer_ = nullptr;
    mx_paddr_t port0_tx_buffer_pa_ = 0;

    // pending iotxns
    list_node iotxn_list = LIST_INITIAL_VALUE(iotxn_list);
};

} // namespace virtio
