// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <kernel/mutex.h>
#include <magenta/thread_annotations.h>
#include <mxtl/intrusive_double_list.h>
#include <mxtl/ref_counted.h>
#include <mxtl/ref_ptr.h>

#include <stdbool.h>
#include <sys/types.h>
#include <inttypes.h>

#define IOMMU_FLAG_PERM_READ    (1<<0)
#define IOMMU_FLAG_PERM_WRITE   (1<<1)
#define IOMMU_FLAG_PERM_EXECUTE (1<<2)

// Type used to refer to virtual addresses presented to a device by the IOMMU.
typedef uint64_t dev_vaddr_t;

class Iommu : public mxtl::RefCounted<Iommu>,
              public mxtl::DoublyLinkedListable<mxtl::RefPtr<Iommu>> {
public:
    // Retrieve a handle to an IOMMU from it's arch-specific identifier.
    //
    // Returns nullptr if the requested one could not be found.
    static mxtl::RefPtr<Iommu> Get(uint64_t iommu_id);

    // Check if |bus_txn_id| is valid for this IOMMU (i.e. could be used
    // to configure a device).
    virtual bool IsValidBusTxnId(uint64_t bus_txn_id) const = 0;

    // Grant the device identified by |bus_txn_id| access to the range of
    // physical addresses given by [paddr, paddr + size).  The base of the
    // mapped range is returned via |vaddr|.  |vaddr| must not be NULL.
    //
    // |perms| defines the access permissions, using the IOMMU_FLAG_PERM_*
    // flags.
    //
    // Returns ERR_INVALID_ARGS if:
    //  |size| is not a multiple of PAGE_SIZE
    //  |paddr| is not aligned to PAGE_SIZE
    // Returns ERR_NOT_FOUND if |bus_txn_id| is not valid.
    virtual status_t Map(uint64_t bus_txn_id, paddr_t paddr, size_t size, uint32_t perms,
                         dev_vaddr_t* vaddr) = 0;

    // Revoke access to the range of addresses [vaddr, vaddr + size) for the
    // device identified by |bus_txn_id|.
    //
    // Returns ERR_INVALID_ARGS if:
    //  |size| is not a multiple of PAGE_SIZE
    //  |vaddr| is not aligned to PAGE_SIZE
    // Returns ERR_NOT_FOUND if |bus_txn_id| is not valid.
    virtual status_t Unmap(uint64_t bus_txn_id, dev_vaddr_t vaddr, size_t size) = 0;

    // Remove all mappings for |bus_txn_id|.
    // Returns ERR_NOT_FOUND if |bus_txn_id| is not valid.
    virtual status_t ClearMappingsForBusTxnId(uint64_t bus_txn_id) = 0;

    // Get the ID assigned to this IOMMU
    uint64_t id() const { return id_; }

    virtual ~Iommu();
protected:
    explicit Iommu(uint64_t id);

    // Register a newly create IOMMU so that it can be retrieved with Get()
    static void RegisterIommu(mxtl::RefPtr<Iommu> iommu);

    // A unique identifier assigned by the implementation.
    const uint64_t id_;
private:

    // Bookkeeping used for Get()
    static Mutex kIommuListLock_;
    static mxtl::DoublyLinkedList<mxtl::RefPtr<Iommu>> kIommuList_ TA_GUARDED(kIommuListLock_);
};
