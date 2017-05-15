// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <magenta/bus_transaction_initiator_dispatcher.h>

#include <dev/iommu.h>
#include <err.h>
#include <kernel/auto_lock.h>
#include <kernel/vm/vm_object.h>
#include <mxalloc/new.h>

constexpr mx_rights_t kDefaultBtiRights =
    MX_RIGHT_DUPLICATE | MX_RIGHT_TRANSFER | MX_RIGHT_READ | MX_RIGHT_MAP;

status_t BusTransactionInitiatorDispatcher::Create(mxtl::RefPtr<Iommu> iommu, uint64_t bti_id,
                                                   mxtl::RefPtr<Dispatcher>* dispatcher,
                                                   mx_rights_t* rights) {

    if (!iommu->IsValidBusTxnId(bti_id)) {
        return ERR_INVALID_ARGS;
    }

    AllocChecker ac;
    auto disp = new (&ac) BusTransactionInitiatorDispatcher(mxtl::move(iommu), bti_id);
    if (!ac.check()) {
        return ERR_NO_MEMORY;
    }

    *rights = kDefaultBtiRights;
    *dispatcher = mxtl::AdoptRef<Dispatcher>(disp);
    return NO_ERROR;
}

BusTransactionInitiatorDispatcher::BusTransactionInitiatorDispatcher(mxtl::RefPtr<Iommu> iommu,
                                                                     uint64_t bti_id)
        : iommu_(mxtl::move(iommu)), bti_id_(bti_id), state_tracker_(0u), zero_handles_(false) {}

BusTransactionInitiatorDispatcher::~BusTransactionInitiatorDispatcher() {
    DEBUG_ASSERT(pinned_memory_.is_empty());
}

status_t BusTransactionInitiatorDispatcher::Pin(mxtl::RefPtr<VmObject> vmo, uint64_t offset,
                                                uint64_t size, uint32_t perms,
                                                dev_vaddr_t* mapped_addrs,
                                                size_t mapped_addrs_len) {
    DEBUG_ASSERT(mapped_addrs);
    if (!IS_PAGE_ALIGNED(offset) || !IS_PAGE_ALIGNED(size)) {
        return ERR_INVALID_ARGS;
    }
    if (mapped_addrs_len < size / PAGE_SIZE) {
        return ERR_BUFFER_TOO_SMALL;
    }

    AutoLock guard(&lock_);

    if (zero_handles_) {
        return ERR_BAD_STATE;
    }

    mxtl::unique_ptr<PinnedMemoryObject> pmo;
    status_t status = PinnedMemoryObject::Create(*this, mxtl::move(vmo),
                                                 offset, size, perms, &pmo);
    if (status != NO_ERROR) {
        return status;
    }

    // Copy out addrs
    DEBUG_ASSERT(pmo->mapped_addrs_len() == size / PAGE_SIZE);
    const auto& pmo_addrs = pmo->mapped_addrs();
    for (size_t i = 0; i < size / PAGE_SIZE; ++i) {
        mapped_addrs[i] = pmo_addrs[i];
    }

    pinned_memory_.push_back(mxtl::move(pmo));
    return NO_ERROR;
}

status_t BusTransactionInitiatorDispatcher::Unpin(const dev_vaddr_t* mapped_addrs,
                                                  size_t mapped_addrs_len) {
    AutoLock guard(&lock_);

    if (zero_handles_) {
        return ERR_BAD_STATE;
    }

    for (auto& pmo : pinned_memory_) {
        if (pmo.mapped_addrs_len() != mapped_addrs_len) {
            continue;
        }

        const auto& pmo_addrs = pmo.mapped_addrs();
        bool match = true;
        for (size_t i = 0; i < mapped_addrs_len ; ++i) {
            if (mapped_addrs[i] != pmo_addrs[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            // The PMO dtor will take care of the actual unpinning.
            pinned_memory_.erase(pmo);
            return NO_ERROR;
        }
    }

    return ERR_INVALID_ARGS;
}

void BusTransactionInitiatorDispatcher::on_zero_handles() {
    AutoLock guard(&lock_);
    while (!pinned_memory_.is_empty()) {
        pinned_memory_.pop_front();
    }
    zero_handles_ = true;
}
