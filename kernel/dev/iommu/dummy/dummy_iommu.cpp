// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <dev/iommu/dummy.h>

#include <err.h>
#include <lk/init.h>
#include <kernel/vm.h>
#include <magenta/resource_dispatcher.h>
#include <mxalloc/new.h>
#include <mxtl/ref_ptr.h>

DummyIommu::DummyIommu(uint64_t id) : Iommu(id) {
}

// TODO: Make this return status_t
mxtl::RefPtr<Iommu> DummyIommu::Create(uint64_t id) {
    AllocChecker ac;
    auto instance = mxtl::AdoptRef<DummyIommu>(new (&ac) DummyIommu(id));
    if (!ac.check()) {
        return nullptr;
    }
    RegisterIommu(instance);
    return instance;
}

DummyIommu::~DummyIommu() {
}

bool DummyIommu::IsValidBusTxnId(uint64_t bus_txn_id) const {
    return true;
}

status_t DummyIommu::Map(uint64_t bus_txn_id, paddr_t paddr, size_t size, uint32_t perms,
                         dev_vaddr_t* vaddr) {
    DEBUG_ASSERT(vaddr);
    if (!IS_PAGE_ALIGNED(paddr) || !IS_PAGE_ALIGNED(size)) {
        return ERR_INVALID_ARGS;
    }
    if (perms & ~(IOMMU_FLAG_PERM_READ | IOMMU_FLAG_PERM_WRITE | IOMMU_FLAG_PERM_EXECUTE)) {
        return ERR_INVALID_ARGS;
    }
    if (perms == 0) {
        return ERR_INVALID_ARGS;
    }
    return NO_ERROR;
}

status_t DummyIommu::Unmap(uint64_t bus_txn_id, dev_vaddr_t vaddr, size_t size) {
    if (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(size)) {
        return ERR_INVALID_ARGS;
    }
    return NO_ERROR;
}

status_t DummyIommu::ClearMappingsForBusTxnId(uint64_t bus_txn_id) {
    return NO_ERROR;
}

status_t DummyIommu::CreateFromResource(mxtl::RefPtr<ResourceDispatcher> rsrc,
                                        mxtl::RefPtr<Iommu>* out) {
    // Dummy IOMMU resources have 0 records
    mx_rrec_t rec;
    if (rsrc->GetNthRecord(0, &rec) != ERR_NOT_FOUND) {
        return ERR_NOT_SUPPORTED;
    }

    mxtl::RefPtr<Iommu> iommu = DummyIommu::Create(rsrc->get_koid());
    if (!iommu) {
        return ERR_NO_MEMORY;
    }
    *out = mxtl::move(iommu);
    return NO_ERROR;
}

IommuDriver DummyIommu::drv_(&DummyIommu::CreateFromResource);
void DummyIommu::RegisterDriver(unsigned int) {
    Iommu::RegisterIommuDriver(&drv_);
}
LK_INIT_HOOK(dummy_iommu_register, DummyIommu::RegisterDriver, LK_INIT_LEVEL_KERNEL);
