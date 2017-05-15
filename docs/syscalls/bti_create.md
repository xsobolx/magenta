# mx_bti_create

## NAME

bti_create - create a new bus transaction initiator

## SYNOPSIS

```
#include <magenta/syscalls.h>

mx_status_t mx_bti_create(mx_handle_t iommu_resource, uint64_t bti_id, mx_handle_t* out);

```

## DESCRIPTION

**bti_create**() creates a new [bus transaction initiator](../objects/bus_transaction_initiator.md)
given a Resource object representing an I/O MMU and a hardware transaction
identifier for a device downstream of that I/O MMU.

Upon success a handle for the new BTI is returned.  This handle will have rights
**MX_RIGHT_READ**, **MX_RIGHT_MAP**, **MX_RIGHT_DUPLICATE**, and
**MX_RIGHT_TRANSFER**.

## RETURN VALUE

**bti_create**() returns NO_ERROR and a handle to the new BTI
(via *out*) on success.  In the event of failure, a negative error value
is returned.

## ERRORS

**ERR_BAD_HANDLE**  *iommu_resource* is not a valid handle.

**ERR_WRONG_TYPE**  *iommu_resource* is not a resource handle.

**ERR_INVALID_ARGS**  *bti_id* is invalid on the given I/O MMU,
or *out* is an invalid pointer.

**ERR_NO_MEMORY**  (Temporary) Failure due to lack of memory.

## SEE ALSO

[bti_pin](bti_pin.md),
[bti_unpin](bti_unpin.md).
