// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <dev/iommu.h>
#include <kernel/mutex.h>
#include <mxtl/canary.h>
#include <mxtl/intrusive_double_list.h>
#include <mxtl/unique_ptr.h>

#include <sys/types.h>

class BusTransactionInitiatorDispatcher;
class VmObject;

class PinnedMemoryObject final : public mxtl::DoublyLinkedListable<mxtl::unique_ptr<PinnedMemoryObject>> {
public:
    // Pin memory in |vmo|'s range [offset, offset+size) on behalf of |bti|,
    // with permissions specified by |perms|.  |perms| should be flags suitable
    // for the Iommu::Map() interface.
    static status_t Create(const BusTransactionInitiatorDispatcher& bti,
                           mxtl::RefPtr<VmObject> vmo, size_t offset,
                           size_t size, uint32_t perms,
                           mxtl::unique_ptr<PinnedMemoryObject>* out);
    ~PinnedMemoryObject();

    // Returns an array of the addresses usable by the given device
    const mxtl::unique_ptr<dev_vaddr_t[]>& mapped_addrs() const { return mapped_addrs_; }
    uint64_t mapped_addrs_len() const { return size_ / PAGE_SIZE; }
private:
    PinnedMemoryObject(const BusTransactionInitiatorDispatcher& bti,
                       mxtl::RefPtr<VmObject> vmo, size_t offset, size_t size,
                       mxtl::unique_ptr<dev_vaddr_t[]> mapped_addrs);

    status_t MapIntoIommu(uint32_t perms);
    status_t UnmapFromIommu();

    mxtl::Canary<mxtl::magic("PMO_")> canary_;

    const mxtl::RefPtr<VmObject> vmo_;
    const uint64_t offset_;
    const uint64_t size_;

    const BusTransactionInitiatorDispatcher& bti_;
    const mxtl::unique_ptr<dev_vaddr_t[]> mapped_addrs_;
};
