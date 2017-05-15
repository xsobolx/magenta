// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT


#include <magenta/pinned_memory_object.h>

#include <assert.h>
#include <err.h>
#include <kernel/auto_lock.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_object.h>
#include <magenta/bus_transaction_initiator_dispatcher.h>
#include <mxalloc/new.h>
#include <mxtl/auto_call.h>

namespace {

struct IommuMapPageContext {
    mxtl::RefPtr<Iommu> iommu;
    uint64_t bus_txn_id;
    dev_vaddr_t* page_array;
    uint32_t perms;
};

// Callback for VmObject::Lookup that handles mapping individual pages into the IOMMU.
status_t IommuMapPage(void* context, size_t offset, size_t index, paddr_t pa) {
    IommuMapPageContext* ctx = static_cast<IommuMapPageContext*>(context);

    dev_vaddr_t vaddr;
    status_t status = ctx->iommu->Map(ctx->bus_txn_id, pa, PAGE_SIZE, ctx->perms, &vaddr);
    if (status != NO_ERROR) {
        return status;
    }

    DEBUG_ASSERT(vaddr != UINT64_MAX);
    ctx->page_array[index] = vaddr;
    return NO_ERROR;
}

} // namespace {}

status_t PinnedMemoryObject::Create(const BusTransactionInitiatorDispatcher& bti,
                                    mxtl::RefPtr<VmObject> vmo, size_t offset,
                                    size_t size, uint32_t perms,
                                    mxtl::unique_ptr<PinnedMemoryObject>* out) {

    DEBUG_ASSERT(IS_PAGE_ALIGNED(offset));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));

    // Pin the memory to make sure it doesn't change from underneath us for the
    // lifetime of the created PMO.
    status_t status = vmo->Pin(offset, size);
    if (status != NO_ERROR) {
        return status;
    }

    // Set up a cleanup function to undo the pin if we need to fail this
    // operation.
    auto unpin_vmo = mxtl::MakeAutoCall([vmo, offset, size]() {
        vmo->Unpin(offset, size);
    });

    AllocChecker ac;
    const size_t num_pages = size / PAGE_SIZE;
    mxtl::unique_ptr<dev_vaddr_t[]> page_array(new (&ac) dev_vaddr_t[num_pages]);
    if (!ac.check()) {
        return ERR_NO_MEMORY;
    }

    mxtl::unique_ptr<PinnedMemoryObject> pmo(
            new (&ac) PinnedMemoryObject(bti, mxtl::move(vmo), offset, size,
                                         mxtl::move(page_array)));
    if (!ac.check()) {
        return ERR_NO_MEMORY;
    }

    // Now that the pmo object has been created, it is responsible for
    // unpinning.
    unpin_vmo.cancel();

    status = pmo->MapIntoIommu(perms);
    if (status != NO_ERROR) {
        return status;
    }

    *out = mxtl::move(pmo);
    return NO_ERROR;
}

// Used during initialization to set up the IOMMU state for this PMO.
status_t PinnedMemoryObject::MapIntoIommu(uint32_t perms) {
    IommuMapPageContext context = {
        .iommu = bti_.iommu(),
        .bus_txn_id = bti_.bti_id(),
        .page_array = mapped_addrs_.get(),
        .perms = perms,
    };
    status_t status = vmo_->Lookup(offset_, size_, 0, IommuMapPage, static_cast<void*>(&context));
    if (status != NO_ERROR) {
        status_t err = UnmapFromIommu();
        ASSERT(err == NO_ERROR);
        return status;
    }

    return NO_ERROR;
}

status_t PinnedMemoryObject::UnmapFromIommu() {
    auto iommu = bti_.iommu();
    const uint64_t bus_txn_id = bti_.bti_id();

    status_t status = NO_ERROR;
    for (size_t i = 0; i < size_ / PAGE_SIZE; ++i) {
        if (mapped_addrs_[i] == UINT64_MAX) {
            break;
        }

        // Try to unmap all pages even if we get an error, and return the
        // first error encountered.
        status_t err = iommu->Unmap(bus_txn_id, mapped_addrs_[i], PAGE_SIZE);
        if (err != NO_ERROR && status == NO_ERROR) {
            status = err;
        }
    }

    return status;
}

PinnedMemoryObject::~PinnedMemoryObject() {
    status_t status = UnmapFromIommu();
    ASSERT(status == NO_ERROR);
    vmo_->Unpin(offset_, size_);
}

PinnedMemoryObject::PinnedMemoryObject(const BusTransactionInitiatorDispatcher& bti,
                                       mxtl::RefPtr<VmObject> vmo, size_t offset, size_t size,
                                       mxtl::unique_ptr<dev_vaddr_t[]> mapped_addrs)
    : vmo_(mxtl::move(vmo)), offset_(offset), size_(size), bti_(bti),
      mapped_addrs_(mxtl::move(mapped_addrs)) {

    // Initialize page array with an invalid address, so we can easily clean it up
    // later if MapIntoIommu() partially fails.
    for (size_t i = 0; i < size_ / PAGE_SIZE; ++i) {
        mapped_addrs_[i] = UINT64_MAX;
    }
}
