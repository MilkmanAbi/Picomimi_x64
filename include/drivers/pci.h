/**
 * Picomimi-x64 PCI Driver Header
 */
#ifndef _DRIVERS_PCI_H
#define _DRIVERS_PCI_H

#include <kernel/types.h>

// ============================================================================
// PCI CONFIGURATION SPACE OFFSETS
// ============================================================================

#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

// PCI Command bits
#define PCI_COMMAND_IO          0x0001
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_COMMAND_MASTER      0x0004

// Class codes
#define PCI_CLASS_STORAGE       0x01
#define PCI_CLASS_NETWORK       0x02
#define PCI_CLASS_DISPLAY       0x03
#define PCI_CLASS_BRIDGE        0x06
#define PCI_CLASS_SERIAL        0x0C

// Match any device
#define PCI_ID_ANY          0xFFFF
#define PCI_ANY_ID          PCI_ID_ANY

// ============================================================================
// PCI DEVICE
// ============================================================================

typedef struct pci_device {
    u8      bus;
    u8      slot;
    u8      func;
    
    u16     vendor_id;
    u16     device_id;
    u16     subsystem_vendor;
    u16     subsystem_id;
    
    u8      revision;
    u8      prog_if;
    u8      subclass;
    u8      class_code;
    
    u8      header_type;
    u8      interrupt_line;
    u8      interrupt_pin;
    
    u64     bar[6];
    u64     bar_size[6];
    u8      bar_type[6];
    
    struct pci_driver *driver;
    void    *driver_data;
    
    struct pci_device *next;
} pci_device_t;

// Device ID for matching
typedef struct pci_device_id {
    u16     vendor;
    u16     device;
    u16     subvendor;
    u16     subdevice;
    u32     class_code;
    u32     class_mask;
    void    *driver_data;
} pci_device_id_t;

#define PCI_DEVICE(vend, dev)   .vendor = (vend), .device = (dev), \
                                .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

// PCI driver structure
typedef struct pci_driver {
    const char *name;
    const pci_device_id_t *id_table;
    int (*probe)(pci_device_t *dev, const pci_device_id_t *id);
    void (*remove)(pci_device_t *dev);
    int (*suspend)(pci_device_t *dev);
    int (*resume)(pci_device_t *dev);
    struct pci_driver *next;
} pci_driver_t;

// ============================================================================
// FUNCTIONS
// ============================================================================

// Configuration access
u8 pci_read_config8(u8 bus, u8 slot, u8 func, u8 offset);
u16 pci_read_config16(u8 bus, u8 slot, u8 func, u8 offset);
u32 pci_read_config32(u8 bus, u8 slot, u8 func, u8 offset);
void pci_write_config8(u8 bus, u8 slot, u8 func, u8 offset, u8 value);
void pci_write_config16(u8 bus, u8 slot, u8 func, u8 offset, u16 value);
void pci_write_config32(u8 bus, u8 slot, u8 func, u8 offset, u32 value);

// Device accessors
u8 pci_dev_read8(pci_device_t *dev, u8 offset);
u16 pci_dev_read16(pci_device_t *dev, u8 offset);
u32 pci_dev_read32(pci_device_t *dev, u8 offset);
void pci_dev_write8(pci_device_t *dev, u8 offset, u8 value);
void pci_dev_write16(pci_device_t *dev, u8 offset, u16 value);
void pci_dev_write32(pci_device_t *dev, u8 offset, u32 value);

// Driver registration
int pci_register_driver(pci_driver_t *driver);
void pci_unregister_driver(pci_driver_t *driver);

// Device control
void pci_enable_device(pci_device_t *dev);
void pci_disable_device(pci_device_t *dev);
void pci_set_master(pci_device_t *dev);

// Device lookup
pci_device_t *pci_get_device(u16 vendor, u16 device, pci_device_t *from);
pci_device_t *pci_get_class(u32 class_code, pci_device_t *from);

// Init
void pci_init(void);

#endif // _DRIVERS_PCI_H
