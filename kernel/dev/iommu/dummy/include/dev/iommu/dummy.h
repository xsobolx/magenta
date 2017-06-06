// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <dev/iommu.h>

class DummyIommu final : public Iommu {
public:
    static mxtl::RefPtr<Iommu> Create(uint64_t id);

    bool IsValidBusTxnId(uint64_t bus_txn_id) const final;

    status_t Map(uint64_t bus_txn_id, paddr_t paddr, size_t size, uint32_t perms,
                 dev_vaddr_t* vaddr) final;
    status_t Unmap(uint64_t bus_txn_id, dev_vaddr_t vaddr, size_t size) final;

    status_t ClearMappingsForBusTxnId(uint64_t bus_txn_id) final;

    ~DummyIommu() final;

    static void RegisterDriver(unsigned int);
private:
    explicit DummyIommu(uint64_t id);

    static status_t CreateFromResource(mxtl::RefPtr<ResourceDispatcher> rsrc,
                                       mxtl::RefPtr<Iommu>* out);
    static IommuDriver drv_;
};
