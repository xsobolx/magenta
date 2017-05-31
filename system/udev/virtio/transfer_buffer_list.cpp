// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "transfer_buffer_list.h"

#include <magenta/errors.h>
#include <mx/vmar.h>

#include "utils.h"
#include "trace.h"

#define LOCAL_TRACE 0

namespace virtio {

TransferBufferList::TransferBufferList() {
}

mx_status_t TransferBufferList::Init(size_t count, size_t buffer_size) {
    assert(count_ == 0);

    count_ = count;
    buffer_size_ = buffer_size;
    size_ = count_ * buffer_size_;

    // allocate a buffer large enough to be able to be carved up into count_
    // buffers of buffer_size_
    mx_status_t err = map_contiguous_memory(size_, (uintptr_t*)&buffer_, &buffer_pa_);
    if (err < 0) {
        VIRTIO_ERROR("cannot alloc buffers %d\n", err);
        return err;
    }

    // build a per buffer descriptor object
    auto tb = new TransferBuffer[count_];
    buffers_.reset(tb, count_);

    for (size_t i = 0; i < count_; i++) {
        buffers_[i].ptr = buffer_ + i * buffer_size;
        buffers_[i].pa = buffer_pa_ + i * buffer_size;
        buffers_[i].total_len = buffer_size;
        buffers_[i].used_len = 0;
        buffers_[i].processed_len = 0;
    }

    return NO_ERROR;
}

TransferBufferList::~TransferBufferList() {
    if (buffer_) {
        mx::vmar::root_self().unmap((uintptr_t)buffer_, size_);
    }
    delete[] buffers_.get();
    buffers_.release();
}

TransferBuffer* TransferBufferList::GetBuffer(size_t index) {
    if (index > count_) {
        assert(0);
        return nullptr;
    }

    return &buffers_[index];
}

TransferBuffer* TransferBufferList::PhysicalToTransfer(mx_paddr_t pa) {
    if (pa < buffer_pa_ || pa >= buffer_pa_ + size_) {
        assert(0);
        return nullptr;
    }

    size_t index = (pa - buffer_pa_) / buffer_size_;
    assert(index < count_);

    LTRACEF("pa %#lx buffer_pa %#lx index %zu\n", pa, buffer_pa_, index);

    return &buffers_[index];
}

} // namespace virtio
