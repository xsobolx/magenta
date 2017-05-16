// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <dev/iommu.h>

#include <kernel/auto_lock.h>
#include <mxtl/intrusive_double_list.h>

Mutex Iommu::kIommuListLock_;
mxtl::DoublyLinkedList<mxtl::RefPtr<Iommu>> Iommu::kIommuList_;

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
