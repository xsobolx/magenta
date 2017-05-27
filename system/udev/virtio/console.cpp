// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "console.h"

#include <ddk/protocol/block.h>
#include <inttypes.h>
#include <magenta/compiler.h>
#include <mxtl/algorithm.h>
#include <mxtl/auto_lock.h>
#include <pretty/hexdump.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "trace.h"
#include "utils.h"

#define LOCAL_TRACE 1

// clang-format off
#define VIRTIO_CONSOLE_F_SIZE (1<<0)
#define VIRTIO_CONSOLE_F_MULTIPORT (1<<1)
#define VIRTIO_CONSOLE_F_EMERG_WRITE (1<<2)

#define VIRTIO_CONSOLE_DEVICE_READY  0
#define VIRTIO_CONSOLE_DEVICE_ADD    1
#define VIRTIO_CONSOLE_DEVICE_REMOVE 2
#define VIRTIO_CONSOLE_PORT_READY    3
#define VIRTIO_CONSOLE_CONSOLE_PORT  4
#define VIRTIO_CONSOLE_RESIZE        5
#define VIRTIO_CONSOLE_PORT_OPEN     6
#define VIRTIO_CONSOLE_PORT_NAME     7

struct virtio_console_control {
    uint32_t id;
    uint16_t event;
    uint16_t value;
};

// clang-format on

namespace virtio {

// DDK level ops
mx_status_t ConsoleDevice::virtio_console_read(void* ctx, void* buf, size_t count, mx_off_t off, size_t* actual) {
    LTRACEF("count %zu off %lu\n", count, off);

    return ERR_NOT_SUPPORTED;
}

mx_status_t ConsoleDevice::virtio_console_write(void* ctx, const void* buf, size_t count, mx_off_t off, size_t* actual) {
    LTRACEF("count %zu off %lu\n", count, off);

    return ERR_NOT_SUPPORTED;
}

ConsoleDevice::ConsoleDevice(mx_driver_t* driver, mx_device_t* bus_device)
    : Device(driver, bus_device) {
    // so that Bind() knows how much io space to allocate
    bar0_size_ = 0x40;
}

ConsoleDevice::~ConsoleDevice() {
    // TODO: clean up allocated physical memory
}

int ConsoleDevice::virtio_console_start_entry(void* arg) {

    ConsoleDevice* c = static_cast<ConsoleDevice*>(arg);

    c->virtio_console_start();

    return 0;
}

static mx_status_t QueueTransfer(Ring *ring, mx_paddr_t pa, uint32_t len, bool write) {
    uint16_t i;
    auto desc = ring->AllocDescChain(1, &i);
    if (!desc) {
        return ERR_NO_MEMORY;
    }

    desc->addr = pa;
    desc->len = len;
    desc->flags = write ? 0 : VRING_DESC_F_WRITE;
#if LOCAL_TRACE > 0
    virtio_dump_desc(desc);
#endif

    /* submit the transfer */
    ring->SubmitChain(i);

    /* kick it off */
    ring->Kick();

    return NO_ERROR;
}

mx_status_t ConsoleDevice::virtio_console_start() {

    mxtl::AutoLock a(&request_lock_);

    // queue a rx on port 0
    QueueTransfer(port_rx_ring_[0].get(), port0_rx_buffer_pa_, port_buffer_size, false);

    // queue a rx on the control port
    QueueTransfer(&control_rx_vring_, control_rx_buffer_pa_, port_buffer_size, false);

    // tell the device we're ready to talk
    virtio_console_control control = {};
    control.event = VIRTIO_CONSOLE_DEVICE_READY;
    control.value = 1;
    memcpy(control_tx_buffer_, &control, sizeof(control));
    QueueTransfer(&control_tx_vring_, control_tx_buffer_pa_, sizeof(control), true);

    return NO_ERROR;
}

mx_status_t ConsoleDevice::Init() {
    LTRACE_ENTRY;

    // reset the device
    Reset();

    // read our configuration
    CopyDeviceConfig(&config_, sizeof(config_));

    LTRACEF("cols %hu\n", config_.cols);
    LTRACEF("rows %hu\n", config_.rows);
    LTRACEF("max_ports %u\n", config_.max_ports);

    // ack and set the driver status bit
    StatusAcknowledgeDriver();

    // XXX check features bits and ack/nak them

    // allocate the control vrings
    mx_status_t err = control_rx_vring_.Init(2, 32);
    if (err < 0) {
        VIRTIO_ERROR("failed to allocate rx control ring\n");
        return err;
    }
    err = control_tx_vring_.Init(3, 32);
    if (err < 0) {
        VIRTIO_ERROR("failed to allocate tx control ring\n");
        return err;
    }

    // port 0 is always present
    port_rx_ring_[0].reset(new Ring(this));
    port_tx_ring_[0].reset(new Ring(this));

    err = port_rx_ring_[0]->Init(0, 128);
    if (err < 0) {
        VIRTIO_ERROR("failed to allocate port rx ring\n");
        return err;
    }
    err = port_tx_ring_[0]->Init(1, 128);
    if (err < 0) {
        VIRTIO_ERROR("failed to allocate port tx ring\n");
        return err;
    }

    // allocate some memory to back our transfers
    size_t size = port_buffer_size + port_buffer_size * 2;
    err = map_contiguous_memory(size, (uintptr_t*)&transfer_buffer_, &transfer_buffer_pa_);
    if (err < 0) {
        VIRTIO_ERROR("cannot alloc buffers %d\n", err);
        return err;
    }

    control_rx_buffer_ = transfer_buffer_;
    control_rx_buffer_pa_ = transfer_buffer_pa_;
    control_tx_buffer_ = control_rx_buffer_ + port_buffer_size;
    control_tx_buffer_pa_ = control_rx_buffer_pa_ + port_buffer_size;
    port0_rx_buffer_ = control_tx_buffer_ + port_buffer_size;
    port0_rx_buffer_pa_ = control_tx_buffer_pa_ + port_buffer_size;
    port0_tx_buffer_ = control_rx_buffer_ + port_buffer_size;
    port0_tx_buffer_pa_ = control_rx_buffer_pa_ + port_buffer_size;

    // start the interrupt thread
    StartIrqThread();

    // set DRIVER_OK
    StatusDriverOK();

    //device_ops_.read = console_read;
    //device_ops_.write = console_write;

    // add the root device under /dev/class/console/virtiocon
    // point the ctx of our DDK device at ourself
    device_add_args_t args = {};
    args.version = DEVICE_ADD_ARGS_VERSION;
    args.name = "virtiocon";
    args.ctx = this;
    args.driver = driver_;
    args.ops = &device_ops_;
    args.proto_id = MX_PROTOCOL_CONSOLE;

    auto status = device_add(bus_device_, &args, &device_);
    if (status < 0) {
        VIRTIO_ERROR("failed device add %d\n", status);
        device_ = nullptr;
        return status;
    }

    // start a worker thread that runs through a sequence to finish initializing the console
    thrd_create_with_name(&start_thread_, virtio_console_start_entry, this, "virtio-console-starter");
    thrd_detach(start_thread_);

    return NO_ERROR;
}

static void complete_transfer(Ring *ring, vring_used_elem *elem) {

    uint32_t i = (uint16_t)elem->id;
    vring_desc* desc = ring->DescFromIndex((uint16_t)i);
    for (;;) {
        int next;

#if LOCAL_TRACE > 0
        virtio_dump_desc(desc);
#endif

        if (desc->flags & VRING_DESC_F_NEXT) {
            next = desc->next;
        } else {
            /* end of chain */
            next = -1;
        }

        ring->FreeDesc((uint16_t)i);

        if (next < 0)
            break;
        i = next;
        desc = ring->DescFromIndex((uint16_t)i);
    }
}

void ConsoleDevice::HandleControlMessage(size_t len) {
    virtio_console_control *rx_message = (virtio_console_control *)control_rx_buffer_;

    bool send_response = false;
    virtio_console_control response  = {};
    switch (rx_message->event) {
        case VIRTIO_CONSOLE_DEVICE_ADD: {
            LTRACEF("CONSOLE_DEVICE_ADD: port %u\n", rx_message->id);
            response.event = VIRTIO_CONSOLE_PORT_READY;
            response.value = 1;
            response.id = rx_message->id;
            send_response = true;

            // add the subdevice for port 0
            if (rx_message->id != 0)
                break; // only can handle port 0 right now

            char name[128];
            snprintf(name, sizeof(name), "virtiocon-%u\n", rx_message->id);

            port0_device_ops_ = {};
            port0_device_ops_.version = DEVICE_OPS_VERSION;
            port0_device_ops_.read = virtio_console_read;
            port0_device_ops_.write = virtio_console_write;

            device_add_args_t args = {};
            args.version = DEVICE_ADD_ARGS_VERSION;
            args.name = name;
            args.ctx = this;
            args.driver = driver_;
            args.ops = &port0_device_ops_;
            args.proto_id = MX_PROTOCOL_CONSOLE;

            mx_status_t status = device_add(device_, &args, &port0_device_);
            if (status < 0) {
                VIRTIO_ERROR("failed device add %d\n", status);
            }
            break;
        }
        case VIRTIO_CONSOLE_CONSOLE_PORT:
            LTRACEF("CONSOLE_CONSOLE_PORT: port %u\n", rx_message->id);
            response.event = VIRTIO_CONSOLE_PORT_OPEN;
            response.value = 1;
            response.id = rx_message->id;
            send_response = true;
            break;
        default:
            TRACEF("unhandled console control message %u\n", rx_message->event);
            hexdump(rx_message, len);
    }

    if (send_response) {
        memcpy(control_tx_buffer_, &response, sizeof(response));
        QueueTransfer(&control_tx_vring_, control_tx_buffer_pa_, sizeof(virtio_console_control), true);
    }
}

void ConsoleDevice::IrqRingUpdate() {
    LTRACE_ENTRY;

    mxtl::AutoLock a(&request_lock_);

    // handle console tx ring transfers
    auto handle_console_tx_ring = [this](vring_used_elem* used_elem) {
        LTRACEF("console tx used_elem %p\n", used_elem);
        complete_transfer(&control_tx_vring_, used_elem);
    };
    control_tx_vring_.IrqRingUpdate(handle_console_tx_ring);

    // handle port 0 tx ring transfers
    auto handle_port0_tx_ring = [this](vring_used_elem* used_elem) {
        LTRACEF("port0 tx used_elem %p\n", used_elem);
        complete_transfer(port_tx_ring_[0].get(), used_elem);
    };
    port_tx_ring_[0]->IrqRingUpdate(handle_port0_tx_ring);

    // handle console rx ring transfers
    auto handle_console_rx_ring = [this](vring_used_elem* used_elem) {
        LTRACEF("console_rx used_elem %p\n", used_elem);
        complete_transfer(&control_rx_vring_, used_elem);

        LTRACEF("control rx len %u\n", used_elem->len);

        HandleControlMessage(used_elem->len);

        QueueTransfer(&control_rx_vring_, control_rx_buffer_pa_, port_buffer_size, false);
    };
    control_rx_vring_.IrqRingUpdate(handle_console_rx_ring);

    // handle port 0 rx ring transfers
    auto handle_port0_rx_ring = [this](vring_used_elem* used_elem) {
        LTRACEF("port0 rx used_elem %p\n", used_elem);
        complete_transfer(port_rx_ring_[0].get(), used_elem);

        LTRACEF("port0 rx len %u\n", used_elem->len);
        hexdump8(port0_rx_buffer_, used_elem->len);

        QueueTransfer(port_rx_ring_[0].get(), port0_rx_buffer_pa_, port_buffer_size, false);
    };
    port_rx_ring_[0]->IrqRingUpdate(handle_port0_rx_ring);

}

void ConsoleDevice::IrqConfigChange() {
    LTRACE_ENTRY;
}

} // namespace virtio
