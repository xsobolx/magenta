#pragma once

struct acpi_pci_table {
    void* ecam;
    size_t ecam_size;
    bool has_legacy;
    bool pci_probed;
};

struct acpi_pci_table* get_magenta_pci_table(void);
