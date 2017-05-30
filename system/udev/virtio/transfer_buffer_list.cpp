// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "transfer_buffer_list.h"

#include <magenta/errors.h>
#include <mx/vmar.h>

#include "utils.h"
#include "trace.h"

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

    return NO_ERROR;
}

TransferBufferList::~TransferBufferList() {
    if (buffer_) {
        mx::vmar::root_self().unmap((uintptr_t)buffer_, size_);
    }
}

mx_status_t TransferBufferList::GetBuffer(size_t index, void **ptr, mx_paddr_t *pa) {
    if (index > count_) {
        assert(0);
        return ERR_INVALID_ARGS;
    }

    if (ptr)
        *ptr = buffer_ + index * buffer_size_;
    if (pa)
        *pa = buffer_pa_ + index * buffer_size_;

    return NO_ERROR;
}

void* TransferBufferList::PhysicalToVirtual(mx_paddr_t pa) {
    // based on the physical address, look up the corresponding virtual address
    if (pa < buffer_pa_ || pa >= buffer_pa_ + size_) {
        assert(0);
        return nullptr;
    }

    return buffer_ + (pa - buffer_pa_);
}

} // namespace virtio
