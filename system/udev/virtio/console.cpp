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

#define LOCAL_TRACE 0

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

    mx_paddr_t pa;

    mxtl::AutoLock a(&request_lock_);

    // queue up all transfers on the control port
    for (size_t i = 0; i < control_ring_size; i++) {
        control_rx_buffers_.GetBuffer(i, nullptr, &pa);
        QueueTransfer(&control_rx_vring_, pa, port_buffer_size, false);
    }

    // tell the device we're ready to talk
    virtio_console_control control = {};
    control.event = VIRTIO_CONSOLE_DEVICE_READY;
    control.value = 1;
    void *control_tx_buffer_;
    control_tx_buffers_.GetBuffer(next_control_tx_buffer_, &control_tx_buffer_, &pa);
    memcpy(control_tx_buffer_, &control, sizeof(control));
    QueueTransfer(&control_tx_vring_, pa, sizeof(control), true);

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
    mx_status_t err = control_rx_vring_.Init(2, control_ring_size);
    if (err < 0) {
        VIRTIO_ERROR("failed to allocate rx control ring\n");
        return err;
    }
    err = control_tx_vring_.Init(3, control_ring_size);
    if (err < 0) {
        VIRTIO_ERROR("failed to allocate tx control ring\n");
        return err;
    }

    control_rx_buffers_.Init(32, control_buffer_size);
    control_tx_buffers_.Init(32, control_buffer_size);

    // start the interrupt thread
    StartIrqThread();

    // set DRIVER_OK
    StatusDriverOK();

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

// return the descriptor chain to the ring, returning the physical address that was used
static mx_paddr_t complete_transfer(Ring *ring, vring_used_elem *elem) {

    uint32_t i = (uint16_t)elem->id;
    vring_desc* desc = ring->DescFromIndex((uint16_t)i);
    mx_paddr_t pa = desc->addr;

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

    return pa;
}

void ConsoleDevice::HandleControlMessage(size_t len, mx_paddr_t pa) {
    virtio_console_control *rx_message = (virtio_console_control *)control_rx_buffers_.PhysicalToVirtual(pa);
    assert(rx_message);

    bool send_response = false;
    virtio_console_control response  = {};
    switch (rx_message->event) {
        case VIRTIO_CONSOLE_DEVICE_ADD: {
            LTRACEF("CONSOLE_DEVICE_ADD: port %u\n", rx_message->id);
            response.event = VIRTIO_CONSOLE_PORT_READY;
            response.value = 1;
            response.id = rx_message->id;
            send_response = true;

            if (port_[rx_message->id].active) {
                TRACEF("CONSOLE_DEVICE_ADD: asked to add port %u which is already active\n", rx_message->id);
                break;
            }

            port_[rx_message->id].Init(this);

            char name[128];
            snprintf(name, sizeof(name), "virtiocon-%u", rx_message->id);

            port_[rx_message->id].device_ops = {};
            port_[rx_message->id].device_ops.version = DEVICE_OPS_VERSION;
            port_[rx_message->id].device_ops.read = virtio_console_read;
            port_[rx_message->id].device_ops.write = virtio_console_write;

            device_add_args_t args = {};
            args.version = DEVICE_ADD_ARGS_VERSION;
            args.name = name;
            args.ctx = this;
            args.driver = driver_;
            args.ops = &port_[rx_message->id].device_ops;
            args.proto_id = MX_PROTOCOL_CONSOLE;

            mx_status_t status = device_add(device_, &args, &port_[rx_message->id].device);
            if (status < 0) {
                VIRTIO_ERROR("failed device add %d\n", status);
            }

            // queue up all the packets on the rx side of the port
            for (size_t i = 0; i < port_ring_size; i++) {
                port_[rx_message->id].rx_buffer.GetBuffer(i, nullptr, &pa);
                QueueTransfer(port_[rx_message->id].rx_ring.get(), pa, port_buffer_size, false);
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
        void* control_tx_buffer;
        mx_paddr_t pa;
        control_tx_buffers_.GetBuffer(next_control_tx_buffer_++, &control_tx_buffer, &pa);
        memcpy(control_tx_buffer, &response, sizeof(response));
        QueueTransfer(&control_tx_vring_, pa, sizeof(virtio_console_control), true);
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

    // handle port tx ring transfers
    for (auto &port: port_) {
        if (port.active) {
            auto handle_port_tx_ring = [this, &port](vring_used_elem* used_elem) {
                LTRACEF("port tx used_elem %p\n", used_elem);
                complete_transfer(port.tx_ring.get(), used_elem);
            };
            port.tx_ring->IrqRingUpdate(handle_port_tx_ring);
        }
    }

    // handle console rx ring transfers
    auto handle_console_rx_ring = [this](vring_used_elem* used_elem) {
        LTRACEF("console_rx used_elem %p\n", used_elem);
        mx_paddr_t pa = complete_transfer(&control_rx_vring_, used_elem);

        LTRACEF("control rx len %u\n", used_elem->len);

        HandleControlMessage(used_elem->len, pa);

        // queue the packet again
        QueueTransfer(&control_rx_vring_, pa, port_buffer_size, false);
    };
    control_rx_vring_.IrqRingUpdate(handle_console_rx_ring);

    // handle port rx ring transfers
    for (auto &port: port_) {
        if (port.active) {
            auto handle_port0_rx_ring = [this, &port](vring_used_elem* used_elem) {
                LTRACEF("port0 rx used_elem %p %u\n", used_elem, used_elem->id);
                mx_paddr_t pa = complete_transfer(port.rx_ring.get(), used_elem);

                LTRACEF("port0 rx len %u\n", used_elem->len);

                // TODO: process incoming port data here

                // queue the rx descriptor again
                QueueTransfer(port.rx_ring.get(), pa, port_buffer_size, false);
            };
            port.rx_ring->IrqRingUpdate(handle_port0_rx_ring);
        }
    }

    LTRACE_EXIT;
}

void ConsoleDevice::IrqConfigChange() {
    LTRACE_ENTRY;
}

mx_status_t ConsoleDevice::Port::Init(Device *dev) {
    if (active)
        return NO_ERROR;

    rx_ring.reset(new Ring(dev));
    tx_ring.reset(new Ring(dev));

    mx_status_t err = rx_ring->Init(0, port_ring_size);
    if (err < 0) {
        VIRTIO_ERROR("failed to allocate port rx ring\n");
        return err;
    }
    err = tx_ring->Init(1, port_ring_size);
    if (err < 0) {
        VIRTIO_ERROR("failed to allocate port tx ring\n");
        return err;
    }

    rx_buffer.Init(port_ring_size, port_buffer_size);
    tx_buffer.Init(port_ring_size, port_buffer_size);

    active = true;

    return NO_ERROR;
}

} // namespace virtio
