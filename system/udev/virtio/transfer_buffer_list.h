// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <magenta/compiler.h>
#include <magenta/types.h>
#include <mxtl/array.h>
#include <mxtl/macros.h>
#include <mxtl/intrusive_double_list.h>
#include <sys/types.h>

#include "trace.h"

namespace virtio {

struct TransferBuffer : mxtl::DoublyLinkedListable<TransferBuffer *> {
    unsigned int index;

    uint8_t* ptr;
    mx_paddr_t pa;
    size_t total_len;

    // modified as transfers are queued and moved around
    size_t used_len;
    size_t processed_len;
};

class TransferBufferList {
public:
    TransferBufferList();
    ~TransferBufferList();

    mx_status_t Init(size_t count, size_t buffer_size);

    TransferBuffer* GetBuffer(size_t index);

    // look up the corresponding transfer based on the physical address
    TransferBuffer* PhysicalToTransfer(mx_paddr_t pa);

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(TransferBufferList);

    size_t count_ = 0;
    size_t buffer_size_ = 0;
    size_t size_ = 0;

    uint8_t* buffer_ = nullptr;
    mx_paddr_t buffer_pa_ = 0;
    mxtl::Array<TransferBuffer> buffers_;
};

class TransferBufferQueue {
public:
    TransferBufferQueue() {}
    ~TransferBufferQueue() {}

    void Add(TransferBuffer *tb) {
        queue_.push_front(tb);
        count_++;
    }

    TransferBuffer* PeekHead() {
        if (queue_.is_empty())
            return nullptr;

        return &queue_.back();
    }

    TransferBuffer* Dequeue() {
        if (queue_.is_empty())
            return nullptr;

        count_--;
        return queue_.pop_back();
    }

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(TransferBufferQueue);

    mxtl::DoublyLinkedList<TransferBuffer*> queue_;
    size_t count_ = 0;
};


} // namespace virtio
