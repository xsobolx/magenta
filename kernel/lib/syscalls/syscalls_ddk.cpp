// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <err.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>

#include <dev/interrupt.h>
#include <dev/iommu.h>
#include <dev/udisplay.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_object_paged.h>
#include <kernel/vm/vm_object_physical.h>
#include <lib/user_copy.h>
#include <lib/user_copy/user_ptr.h>

#if ARCH_X86
#include <platform/pc/bootloader.h>
#endif

#include <magenta/bus_transaction_initiator_dispatcher.h>
#include <magenta/handle_owner.h>
#include <magenta/interrupt_dispatcher.h>
#include <magenta/interrupt_event_dispatcher.h>
#include <magenta/io_mapping_dispatcher.h>
#include <magenta/magenta.h>
#include <magenta/process_dispatcher.h>
#include <magenta/resource_dispatcher.h>
#include <magenta/syscalls/pci.h>
#include <magenta/user_copy.h>
#include <magenta/vm_object_dispatcher.h>
#include <mxtl/auto_call.h>
#include <mxtl/inline_array.h>

#include "syscalls_priv.h"

#define LOCAL_TRACE 0

static_assert(MX_CACHE_POLICY_CACHED == ARCH_MMU_FLAG_CACHED,
              "Cache policy constant mismatch - CACHED");
static_assert(MX_CACHE_POLICY_UNCACHED == ARCH_MMU_FLAG_UNCACHED,
              "Cache policy constant mismatch - UNCACHED");
static_assert(MX_CACHE_POLICY_UNCACHED_DEVICE == ARCH_MMU_FLAG_UNCACHED_DEVICE,
              "Cache policy constant mismatch - UNCACHED_DEVICE");
static_assert(MX_CACHE_POLICY_WRITE_COMBINING == ARCH_MMU_FLAG_WRITE_COMBINING,
              "Cache policy constant mismatch - WRITE_COMBINING");

mx_handle_t sys_interrupt_create(mx_handle_t hrsrc, uint32_t vector, uint32_t options) {
    LTRACEF("vector %u options 0x%x\n", vector, options);

    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    status_t result = InterruptEventDispatcher::Create(vector, options, &dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    HandleOwner handle(MakeHandle(mxtl::move(dispatcher), rights));

    auto up = ProcessDispatcher::GetCurrent();
    mx_handle_t hv = up->MapHandleToValue(handle);
    up->AddHandle(mxtl::move(handle));
    return hv;
}

mx_status_t sys_interrupt_complete(mx_handle_t handle_value) {
    LTRACEF("handle %d\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<InterruptDispatcher> interrupt;
    mx_status_t status = up->GetDispatcher(handle_value, &interrupt);
    if (status != NO_ERROR)
        return status;

    return interrupt->InterruptComplete();
}

mx_status_t sys_interrupt_wait(mx_handle_t handle_value) {
    LTRACEF("handle %d\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<InterruptDispatcher> interrupt;
    mx_status_t status = up->GetDispatcher(handle_value, &interrupt);
    if (status != NO_ERROR)
        return status;

    return interrupt->WaitForInterrupt();
}

mx_status_t sys_interrupt_signal(mx_handle_t handle_value) {
    LTRACEF("handle %d\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<InterruptDispatcher> interrupt;
    mx_status_t status = up->GetDispatcher(handle_value, &interrupt);
    if (status != NO_ERROR)
        return status;

    return interrupt->UserSignal();
}

mx_status_t sys_mmap_device_memory(mx_handle_t hrsrc, uintptr_t paddr, uint32_t len,
                                   mx_cache_policy_t cache_policy,
                                   user_ptr<uintptr_t> _out_vaddr) {

    LTRACEF("addr %#" PRIxPTR " len %#x\n", paddr, len);

    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    if (!_out_vaddr)
        return ERR_INVALID_ARGS;

    uint arch_mmu_flags =
        ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE |
        ARCH_MMU_FLAG_PERM_USER;

    uint vmo_cache_policy;
    switch (cache_policy) {
    case MX_CACHE_POLICY_CACHED:
        vmo_cache_policy = ARCH_MMU_FLAG_CACHED;
        break;
    case MX_CACHE_POLICY_UNCACHED:
        vmo_cache_policy = ARCH_MMU_FLAG_UNCACHED;
        break;
    case MX_CACHE_POLICY_UNCACHED_DEVICE:
        vmo_cache_policy = ARCH_MMU_FLAG_UNCACHED_DEVICE;
        break;
    case MX_CACHE_POLICY_WRITE_COMBINING:
        vmo_cache_policy = ARCH_MMU_FLAG_WRITE_COMBINING;
        break;
    default:
        return ERR_INVALID_ARGS;
    }

    mxtl::RefPtr<VmObject> vmo(VmObjectPhysical::Create(paddr, len));
    if (!vmo) {
        return ERR_NO_MEMORY;
    }

    if (vmo->SetMappingCachePolicy(vmo_cache_policy) != NO_ERROR) {
        return ERR_INVALID_ARGS;
    }

    auto aspace = ProcessDispatcher::GetCurrent()->aspace();
    auto vmar = aspace->RootVmar();

    mxtl::RefPtr<VmMapping> mapping;
    status_t res = vmar->CreateVmMapping(0, len, PAGE_SIZE_SHIFT, 0,
                                         mxtl::move(vmo), 0, arch_mmu_flags, "user_mmio",
                                         &mapping);

    if (res != NO_ERROR) {
        return res;
    }

    // Force the entries into the page tables
    status = mapping->MapRange(0, len, false);
    if (status < 0) {
        mapping->Destroy();
        return status;
    }

    if (_out_vaddr.copy_to_user(
        reinterpret_cast<uintptr_t>(mapping->base())) != NO_ERROR) {
        mapping->Destroy();
        return ERR_INVALID_ARGS;
    }

    return NO_ERROR;
}

mx_status_t sys_vmo_create_contiguous(mx_handle_t hrsrc, size_t size,
                                      uint32_t alignment_log2,
                                      user_ptr<mx_handle_t> _out) {
    LTRACEF("size 0x%zu\n", size);

    if (size == 0) return ERR_INVALID_ARGS;
    if (alignment_log2 == 0)
        alignment_log2 = PAGE_SIZE_SHIFT;
    // catch obviously wrong values
    if (alignment_log2 < PAGE_SIZE_SHIFT ||
            alignment_log2 >= (8 * sizeof(uint64_t)))
        return ERR_INVALID_ARGS;

    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    size = ROUNDUP_PAGE_SIZE(size);
    // create a vm object
    mxtl::RefPtr<VmObject> vmo = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, size);
    if (!vmo)
        return ERR_NO_MEMORY;

    // always immediately commit memory to the object
    uint64_t committed;
    // CommitRangeContiguous takes a uint8_t for the alignment
    auto align_log2_arg = static_cast<uint8_t>(alignment_log2);
    status = vmo->CommitRangeContiguous(0, size, &committed, align_log2_arg);
    if (status < 0 || (size_t)committed < size) {
        LTRACEF("failed to allocate enough pages (asked for %zu, got %zu)\n", size / PAGE_SIZE,
                (size_t)committed / PAGE_SIZE);
        return ERR_NO_MEMORY;
    }

    // create a Vm Object dispatcher
    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    mx_status_t result = VmObjectDispatcher::Create(mxtl::move(vmo), &dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    // create a handle and attach the dispatcher to it
    HandleOwner handle(MakeHandle(mxtl::move(dispatcher), rights));
    if (!handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    if (_out.copy_to_user(up->MapHandleToValue(handle)) != NO_ERROR)
        return ERR_INVALID_ARGS;

    up->AddHandle(mxtl::move(handle));
    return NO_ERROR;
}

mx_status_t sys_bootloader_fb_get_info(user_ptr<uint32_t> format, user_ptr<uint32_t> width, user_ptr<uint32_t> height, user_ptr<uint32_t> stride) {
#if ARCH_X86
    if (!bootloader.fb_base ||
            format.copy_to_user(bootloader.fb_format) ||
            width.copy_to_user(bootloader.fb_width) ||
            height.copy_to_user(bootloader.fb_height) ||
            stride.copy_to_user(bootloader.fb_stride)) {
        return ERR_INVALID_ARGS;
    } else {
        return NO_ERROR;
    }
#else
    return ERR_NOT_SUPPORTED;
#endif
}

mx_status_t sys_set_framebuffer(mx_handle_t hrsrc, user_ptr<void> vaddr, uint32_t len, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    intptr_t paddr = vaddr_to_paddr(vaddr.get());
    udisplay_set_framebuffer(paddr, len);

    struct display_info di;
    memset(&di, 0, sizeof(struct display_info));
    di.format = format;
    di.width = width;
    di.height = height;
    di.stride = stride;
    di.flags = DISPLAY_FLAG_HW_FRAMEBUFFER;
    udisplay_set_display_info(&di);

    return NO_ERROR;
}

mx_status_t sys_set_framebuffer_vmo(mx_handle_t hrsrc, mx_handle_t vmo_handle, uint32_t len, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0)
        return status;

    auto up = ProcessDispatcher::GetCurrent();

    // lookup the dispatcher from handle
    mxtl::RefPtr<VmObjectDispatcher> vmo;
    status = up->GetDispatcher(vmo_handle, &vmo);
    if (status != NO_ERROR)
        return status;

    status = udisplay_set_framebuffer_vmo(vmo->vmo());
    if (status != NO_ERROR)
        return status;

    struct display_info di;
    memset(&di, 0, sizeof(struct display_info));
    di.format = format;
    di.width = width;
    di.height = height;
    di.stride = stride;
    di.flags = DISPLAY_FLAG_HW_FRAMEBUFFER;
    udisplay_set_display_info(&di);

    return NO_ERROR;
}

/**
 * Gets info about an I/O mapping object.
 * @param handle Handle associated with an I/O mapping object.
 * @param out_vaddr Mapped virtual address for the I/O range.
 * @param out_len Mapped size of the I/O range.
 */
mx_status_t sys_io_mapping_get_info(mx_handle_t handle,
                                    user_ptr<uintptr_t> _out_vaddr,
                                    user_ptr<uint64_t> _out_size) {
    LTRACEF("handle %d\n", handle);

    if (!_out_vaddr || !_out_size)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<IoMappingDispatcher> io_mapping;
    mx_status_t status = up->GetDispatcherWithRights(handle, MX_RIGHT_READ, &io_mapping);
    if (status != NO_ERROR)
        return status;

    // If we do not have read rights, or we are calling from a different address
    // space than the one that this mapping exists in, refuse to tell the user
    // the vaddr/len of the mapping.
    if (ProcessDispatcher::GetCurrent()->aspace() != io_mapping->aspace())
        return ERR_ACCESS_DENIED;

    uintptr_t vaddr = reinterpret_cast<uintptr_t>(io_mapping->vaddr());
    uint64_t  size  = io_mapping->size();

    status = _out_vaddr.copy_to_user(vaddr);
    if (status != NO_ERROR)
        return status;

    return _out_size.copy_to_user(size);
}

#if ARCH_X86
#include <arch/x86/descriptor.h>
#include <arch/x86/ioport.h>

mx_status_t sys_mmap_device_io(mx_handle_t hrsrc, uint32_t io_addr, uint32_t len) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    LTRACEF("addr 0x%x len 0x%x\n", io_addr, len);

    return x86_set_io_bitmap(io_addr, len, 1);
}
#else
mx_status_t sys_mmap_device_io(mx_handle_t hrsrc, uint32_t io_addr, uint32_t len) {
    // doesn't make sense on non-x86
    return ERR_NOT_SUPPORTED;
}
#endif

uint64_t sys_acpi_uefi_rsdp(mx_handle_t hrsrc) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }
#if ARCH_X86
    return bootloader.acpi_rsdp;
#endif
    return 0;
}

mx_status_t sys_acpi_cache_flush(mx_handle_t hrsrc) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }
    // TODO(teisenbe): This should be restricted to when interrupts are
    // disabled, but we haven't added support for letting the ACPI process
    // disable interrupts yet.  It only uses this for S-state transitions
    // like poweroff and (more importantly) sleep.
#if ARCH_X86
    __asm__ volatile ("wbinvd");
    return NO_ERROR;
#else
    return ERR_NOT_SUPPORTED;
#endif
}

mx_status_t sys_bti_create(mx_handle_t iommu_resource, uint64_t bti_id, user_ptr<mx_handle_t> out) {
    auto up = ProcessDispatcher::GetCurrent();

    mx_status_t status;
    mxtl::RefPtr<ResourceDispatcher> resource;
    status = up->GetDispatcherWithRights(iommu_resource, MX_RIGHT_EXECUTE, &resource);
    if (status != NO_ERROR) {
        return status;
    }

    mxtl::RefPtr<Iommu> iommu(Iommu::Get(resource->get_koid()));
    if (!iommu) {
        return ERR_INVALID_ARGS;
    }

    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    status = BusTransactionInitiatorDispatcher::Create(mxtl::move(iommu), bti_id,
                                                       &dispatcher, &rights);
    if (status != NO_ERROR) {
        return status;
    }
    HandleOwner handle(MakeHandle(mxtl::move(dispatcher), rights));

    mx_handle_t hv = up->MapHandleToValue(handle);
    if ((status = out.copy_to_user(hv)) != NO_ERROR) {
        return status;
    }

    up->AddHandle(mxtl::move(handle));
    return NO_ERROR;
}

mx_status_t sys_bti_pin(mx_handle_t bti, mx_handle_t vmo, uint64_t offset, uint64_t size,
        uint32_t perms, user_ptr<uint64_t> addrs, uint32_t addrs_len, user_ptr<uint32_t> actual_addrs_len) {
    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<BusTransactionInitiatorDispatcher> bti_dispatcher;
    mx_status_t status = up->GetDispatcherWithRights(bti, MX_RIGHT_MAP, &bti_dispatcher);
    if (status != NO_ERROR) {
        return status;
    }

    mxtl::RefPtr<VmObjectDispatcher> vmo_dispatcher;
    mx_rights_t vmo_rights;
    status = up->GetDispatcherAndRights(vmo, &vmo_dispatcher, &vmo_rights);
    if (status != NO_ERROR) {
        return status;
    }
    if (!(vmo_rights & MX_RIGHT_MAP)) {
        return ERR_ACCESS_DENIED;
    }

    const size_t actual_len = size / PAGE_SIZE;
    if (addrs_len < actual_len) {
        return ERR_BUFFER_TOO_SMALL;
    }

    // Convert requested permissions and check against VMO rights
    uint32_t iommu_perms = 0;
    if (perms & MX_VM_FLAG_PERM_READ) {
        if (!(vmo_rights & MX_RIGHT_READ)) {
            return ERR_ACCESS_DENIED;
        }
        iommu_perms |= IOMMU_FLAG_PERM_READ;
        perms &= ~MX_VM_FLAG_PERM_READ;
    }
    if (perms & MX_VM_FLAG_PERM_WRITE) {
        if (!(vmo_rights & MX_RIGHT_WRITE)) {
            return ERR_ACCESS_DENIED;
        }
        iommu_perms |= IOMMU_FLAG_PERM_WRITE;
        perms &= ~MX_VM_FLAG_PERM_WRITE;
    }
    if (perms & MX_VM_FLAG_PERM_EXECUTE) {
        if (!(vmo_rights & MX_RIGHT_EXECUTE)) {
            return ERR_ACCESS_DENIED;
        }
        iommu_perms |= IOMMU_FLAG_PERM_EXECUTE;
        perms &= ~MX_VM_FLAG_PERM_EXECUTE;
    }
    if (perms) {
        return ERR_INVALID_ARGS;
    }

    AllocChecker ac;
    mxtl::InlineArray<dev_vaddr_t, 4u> mapped_addrs(&ac, actual_len);
    if (!ac.check()) {
        return ERR_NO_MEMORY;
    }

    status = bti_dispatcher->Pin(vmo_dispatcher->vmo(), offset, size, iommu_perms,
                                 mapped_addrs.get(), actual_len);
    if (status != NO_ERROR) {
        return status;
    }

    auto pin_cleanup = mxtl::MakeAutoCall([&bti_dispatcher, &mapped_addrs, actual_len]() {
        bti_dispatcher->Unpin(mapped_addrs.get(), actual_len);
    });

    static_assert(sizeof(dev_vaddr_t) == sizeof(uint64_t), "mismatched types");
    if ((status = addrs.copy_array_to_user(mapped_addrs.get(), actual_len)) != NO_ERROR) {
        return status;
    }
    if ((status = actual_addrs_len.copy_to_user(static_cast<uint32_t>(actual_len))) != NO_ERROR) {
        return status;
    }

    pin_cleanup.cancel();
    return NO_ERROR;
}

mx_status_t sys_bti_unpin(mx_handle_t bti, user_ptr<const uint64_t> addrs, uint32_t addrs_len) {
    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<BusTransactionInitiatorDispatcher> bti_dispatcher;
    mx_status_t status = up->GetDispatcherWithRights(bti, MX_RIGHT_MAP, &bti_dispatcher);
    if (status != NO_ERROR) {
        return status;
    }

    AllocChecker ac;
    mxtl::InlineArray<dev_vaddr_t, 4u> mapped_addrs(&ac, addrs_len);
    if (!ac.check()) {
        return ERR_NO_MEMORY;
    }
    static_assert(sizeof(dev_vaddr_t) == sizeof(uint64_t), "mismatched types");
    if ((status = addrs.copy_array_from_user(mapped_addrs.get(), addrs_len)) != NO_ERROR) {
        return status;
    }

    return bti_dispatcher->Unpin(mapped_addrs.get(), addrs_len);

}
