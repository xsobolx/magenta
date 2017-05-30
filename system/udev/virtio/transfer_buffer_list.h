// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <sys/types.h>
#include <magenta/compiler.h>
#include <magenta/types.h>
#include <mxtl/macros.h>

namespace virtio {

class TransferBufferList {
public:
    TransferBufferList();
    ~TransferBufferList();

    mx_status_t Init(size_t count, size_t buffer_size);

    mx_status_t GetBuffer(size_t index, void **ptr, mx_paddr_t *pa);
    void *PhysicalToVirtual(mx_paddr_t pa);

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(TransferBufferList);

    size_t count_ = 0;
    size_t buffer_size_ = 0;
    size_t size_ = 0;

    uint8_t* buffer_ = nullptr;
    mx_paddr_t buffer_pa_ = 0;
};

} // namespace virtio
