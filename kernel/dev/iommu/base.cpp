// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <dev/iommu.h>

#include <kernel/auto_lock.h>
#include <magenta/resource_dispatcher.h>
#include <mxtl/intrusive_double_list.h>

Mutex Iommu::kIommuListLock_;
mxtl::DoublyLinkedList<mxtl::RefPtr<Iommu>> Iommu::kIommuList_;
mxtl::DoublyLinkedList<IommuDriver*> Iommu::kDriverList_;

Iommu::Iommu(uint64_t iommu_id) : id_(iommu_id) {
}

Iommu::~Iommu() {
}

mxtl::RefPtr<Iommu> Iommu::Get(uint64_t iommu_id) {
    AutoLock guard(&kIommuListLock_);

    for (auto& iommu : kIommuList_) {
        if (iommu.id() == iommu_id) {
            return mxtl::WrapRefPtr(&iommu);
        }
    }
    return nullptr;
}

void Iommu::RegisterIommu(mxtl::RefPtr<Iommu> iommu) {
    AutoLock guard(&kIommuListLock_);

    for (auto& other : kIommuList_) {
        if (other.id() == iommu->id()) {
            panic("Attempted to register two IOMMUs with the same ID\n");
        }
    }

    kIommuList_.push_back(mxtl::move(iommu));
}

status_t Iommu::CreateFromResource(mxtl::RefPtr<ResourceDispatcher> rsrc,
                                   mxtl::RefPtr<Iommu>* out) {
    for (auto& drv : kDriverList_) {
        status_t status = drv.create_from_resource(rsrc, out);
        if (status != ERR_NOT_SUPPORTED) {
            return status;
        }
    }
    return ERR_NOT_SUPPORTED;
}

void Iommu::RegisterIommuDriver(IommuDriver* drv) {
    kDriverList_.push_back(drv);
}
