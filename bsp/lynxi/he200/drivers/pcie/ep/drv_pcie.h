#ifndef __DRV_PCIE_H__
#define __DRV_PCIE_H__

/* PCIe MSI-X interfaces */
int rt_pci_msix_init(void);
void rt_pci_msix_deinit(void);
int rt_pci_one_msix_deinit(int vector);
int rt_pci_msix_register_vector(int vector);
int rt_pci_msix_raise_irq(int vector);

#endif
