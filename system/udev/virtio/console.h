// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include "device.h"
#include "ring.h"

#include <magenta/compiler.h>
#include <mxtl/unique_ptr.h>
#include <stdlib.h>

#include "transfer_buffer_list.h"

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

    void HandleControlMessage(size_t, mx_paddr_t);

    mx_status_t virtio_console_start();
    static int virtio_console_start_entry(void* arg);
    thrd_t start_thread_ = {};

    // request condition
    mxtl::Mutex request_lock_;
    cnd_t request_cond_ = {};

    // control tx/rx rings
    Ring control_rx_vring_ = {this};
    Ring control_tx_vring_ = {this};

    // per port configurations
    struct Port {
        Port() {}

        mx_status_t Init(Device *dev);

        mxtl::unique_ptr<Ring> rx_ring;
        mxtl::unique_ptr<Ring> tx_ring;

        TransferBufferList rx_buffer;
        TransferBufferList tx_buffer;

        mx_device_t *device = nullptr;
        mx_protocol_device_t device_ops = {};

        bool active = false;
    };

    Port port_[32];

    // saved block device configuration out of the pci config BAR
    struct virtio_console_config {
        uint16_t cols;
        uint16_t rows;
        uint32_t max_ports;
        uint32_t emerg_wr;
    } config_ __PACKED = {};

    static const size_t control_buffer_size = 128;
    static const size_t control_ring_size = 32;
    static const size_t port_buffer_size = 512;
    static const size_t port_ring_size = 128;

    // transfer buffers for control rx and tx
    TransferBufferList control_rx_buffers_;
    TransferBufferList control_tx_buffers_;
    uint32_t next_control_tx_buffer_ = 0;

    // pending iotxns
    list_node iotxn_list = LIST_INITIAL_VALUE(iotxn_list);
};

} // namespace virtio
