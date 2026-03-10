/**
 * Picomimi-x64 PCI Bus Driver
 * 
 * PCI configuration space access and device enumeration
 */

#include <kernel/types.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/cpu.h>
#include <arch/io.h>

// ============================================================================
// PCI CONFIGURATION SPACE
// ============================================================================

#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

// Special value for "any" matching (use 0xFFFF for u16 fields)
#define PCI_ID_ANY          0xFFFF

// Configuration space offsets
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_LATENCY_TIMER   0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_CARDBUS_CIS     0x28
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C
#define PCI_SUBSYSTEM_ID    0x2E
#define PCI_ROM_ADDRESS     0x30
#define PCI_CAPABILITIES    0x34
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D
#define PCI_MIN_GNT         0x3E
#define PCI_MAX_LAT         0x3F

// PCI Command Register bits
#define PCI_COMMAND_IO          0x0001
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_COMMAND_MASTER      0x0004
#define PCI_COMMAND_SPECIAL     0x0008
#define PCI_COMMAND_INVALIDATE  0x0010
#define PCI_COMMAND_VGA_PALETTE 0x0020
#define PCI_COMMAND_PARITY      0x0040
#define PCI_COMMAND_WAIT        0x0080
#define PCI_COMMAND_SERR        0x0100
#define PCI_COMMAND_FAST_BACK   0x0200
#define PCI_COMMAND_INTX_DISABLE 0x0400

// Header types
#define PCI_HEADER_TYPE_NORMAL  0x00
#define PCI_HEADER_TYPE_BRIDGE  0x01
#define PCI_HEADER_TYPE_CARDBUS 0x02
#define PCI_HEADER_MULTIFUNCTION 0x80

// BAR types
#define PCI_BAR_IO              0x01
#define PCI_BAR_LOWMEM          0x00
#define PCI_BAR_64BIT           0x04
#define PCI_BAR_PREFETCH        0x08

// Class codes
#define PCI_CLASS_STORAGE       0x01
#define PCI_CLASS_NETWORK       0x02
#define PCI_CLASS_DISPLAY       0x03
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_CLASS_MEMORY        0x05
#define PCI_CLASS_BRIDGE        0x06
#define PCI_CLASS_COMMUNICATION 0x07
#define PCI_CLASS_SYSTEM        0x08
#define PCI_CLASS_INPUT         0x09
#define PCI_CLASS_DOCKING       0x0A
#define PCI_CLASS_PROCESSOR     0x0B
#define PCI_CLASS_SERIAL        0x0C
#define PCI_CLASS_WIRELESS      0x0D
#define PCI_CLASS_INTELLIGENT   0x0E
#define PCI_CLASS_SATELLITE     0x0F
#define PCI_CLASS_ENCRYPTION    0x10
#define PCI_CLASS_SIGNAL        0x11
#define PCI_CLASS_ACCELERATOR   0x12

// ============================================================================
// PCI DEVICE STRUCTURE
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
    
    // BARs
    u64     bar[6];
    u64     bar_size[6];
    u8      bar_type[6];  // 0 = memory, 1 = I/O
    
    // Driver
    struct pci_driver *driver;
    void    *driver_data;
    
    // Linked list
    struct pci_device *next;
} pci_device_t;

// PCI driver structure
typedef struct pci_driver {
    const char *name;
    
    // Device ID table
    const struct pci_device_id *id_table;
    
    // Callbacks
    int (*probe)(pci_device_t *dev, const struct pci_device_id *id);
    void (*remove)(pci_device_t *dev);
    int (*suspend)(pci_device_t *dev);
    int (*resume)(pci_device_t *dev);
    
    struct pci_driver *next;
} pci_driver_t;

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

// Use 0xFFFF defined at top of file
#define PCI_DEVICE(vend, dev)   .vendor = (vend), .device = (dev), \
                                .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY

// ============================================================================
// GLOBAL STATE
// ============================================================================

static pci_device_t *pci_devices = NULL;
static pci_driver_t *pci_drivers = NULL;
static int pci_device_count = 0;

// ============================================================================
// PCI CONFIGURATION ACCESS
// ============================================================================

static inline u32 pci_config_address(u8 bus, u8 slot, u8 func, u8 offset) {
    return (1U << 31) |                // Enable bit
           ((u32)bus << 16) |
           ((u32)slot << 11) |
           ((u32)func << 8) |
           (offset & 0xFC);
}

u8 pci_read_config8(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDR, pci_config_address(bus, slot, func, offset));
    return inb(PCI_CONFIG_DATA + (offset & 3));
}

u16 pci_read_config16(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDR, pci_config_address(bus, slot, func, offset));
    return inw(PCI_CONFIG_DATA + (offset & 2));
}

u32 pci_read_config32(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDR, pci_config_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config8(u8 bus, u8 slot, u8 func, u8 offset, u8 value) {
    outl(PCI_CONFIG_ADDR, pci_config_address(bus, slot, func, offset));
    outb(PCI_CONFIG_DATA + (offset & 3), value);
}

void pci_write_config16(u8 bus, u8 slot, u8 func, u8 offset, u16 value) {
    outl(PCI_CONFIG_ADDR, pci_config_address(bus, slot, func, offset));
    outw(PCI_CONFIG_DATA + (offset & 2), value);
}

void pci_write_config32(u8 bus, u8 slot, u8 func, u8 offset, u32 value) {
    outl(PCI_CONFIG_ADDR, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

// Device accessors
u8 pci_dev_read8(pci_device_t *dev, u8 offset) {
    return pci_read_config8(dev->bus, dev->slot, dev->func, offset);
}

u16 pci_dev_read16(pci_device_t *dev, u8 offset) {
    return pci_read_config16(dev->bus, dev->slot, dev->func, offset);
}

u32 pci_dev_read32(pci_device_t *dev, u8 offset) {
    return pci_read_config32(dev->bus, dev->slot, dev->func, offset);
}

void pci_dev_write8(pci_device_t *dev, u8 offset, u8 value) {
    pci_write_config8(dev->bus, dev->slot, dev->func, offset, value);
}

void pci_dev_write16(pci_device_t *dev, u8 offset, u16 value) {
    pci_write_config16(dev->bus, dev->slot, dev->func, offset, value);
}

void pci_dev_write32(pci_device_t *dev, u8 offset, u32 value) {
    pci_write_config32(dev->bus, dev->slot, dev->func, offset, value);
}

// ============================================================================
// BAR HANDLING
// ============================================================================

static void pci_read_bars(pci_device_t *dev) {
    int max_bars = (dev->header_type & 0x7F) == PCI_HEADER_TYPE_NORMAL ? 6 : 2;
    
    for (int i = 0; i < max_bars; i++) {
        u8 offset = PCI_BAR0 + i * 4;
        u32 bar = pci_dev_read32(dev, offset);
        
        if (bar == 0) {
            dev->bar[i] = 0;
            dev->bar_size[i] = 0;
            continue;
        }
        
        // Determine type
        if (bar & PCI_BAR_IO) {
            // I/O BAR
            dev->bar_type[i] = 1;
            dev->bar[i] = bar & ~0x3;
            
            // Get size
            pci_dev_write32(dev, offset, 0xFFFFFFFF);
            u32 size = pci_dev_read32(dev, offset);
            pci_dev_write32(dev, offset, bar);
            
            size = ~(size & ~0x3) + 1;
            dev->bar_size[i] = size;
        } else {
            // Memory BAR
            dev->bar_type[i] = 0;
            
            if (bar & PCI_BAR_64BIT) {
                // 64-bit BAR
                u32 bar_high = pci_dev_read32(dev, offset + 4);
                dev->bar[i] = ((u64)bar_high << 32) | (bar & ~0xF);
                
                // Get size
                pci_dev_write32(dev, offset, 0xFFFFFFFF);
                pci_dev_write32(dev, offset + 4, 0xFFFFFFFF);
                u32 size_low = pci_dev_read32(dev, offset);
                u32 size_high = pci_dev_read32(dev, offset + 4);
                pci_dev_write32(dev, offset, bar);
                pci_dev_write32(dev, offset + 4, bar_high);
                
                u64 size = ((u64)size_high << 32) | (size_low & ~0xF);
                dev->bar_size[i] = ~size + 1;
                
                i++;  // Skip next BAR (part of 64-bit)
                dev->bar[i] = 0;
                dev->bar_size[i] = 0;
            } else {
                // 32-bit BAR
                dev->bar[i] = bar & ~0xF;
                
                // Get size
                pci_dev_write32(dev, offset, 0xFFFFFFFF);
                u32 size = pci_dev_read32(dev, offset);
                pci_dev_write32(dev, offset, bar);
                
                size = ~(size & ~0xF) + 1;
                dev->bar_size[i] = size;
            }
        }
    }
}

// ============================================================================
// DEVICE ENUMERATION
// ============================================================================

static pci_device_t *pci_probe_device(u8 bus, u8 slot, u8 func) {
    u16 vendor = pci_read_config16(bus, slot, func, PCI_VENDOR_ID);
    
    if (vendor == 0xFFFF) {
        return NULL;  // No device
    }
    
    pci_device_t *dev = kmalloc(sizeof(pci_device_t), GFP_KERNEL);
    if (!dev) return NULL;
    
    memset(dev, 0, sizeof(*dev));
    
    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    
    dev->vendor_id = vendor;
    dev->device_id = pci_read_config16(bus, slot, func, PCI_DEVICE_ID);
    dev->revision = pci_read_config8(bus, slot, func, PCI_REVISION_ID);
    dev->prog_if = pci_read_config8(bus, slot, func, PCI_PROG_IF);
    dev->subclass = pci_read_config8(bus, slot, func, PCI_SUBCLASS);
    dev->class_code = pci_read_config8(bus, slot, func, PCI_CLASS);
    dev->header_type = pci_read_config8(bus, slot, func, PCI_HEADER_TYPE);
    dev->interrupt_line = pci_read_config8(bus, slot, func, PCI_INTERRUPT_LINE);
    dev->interrupt_pin = pci_read_config8(bus, slot, func, PCI_INTERRUPT_PIN);
    
    if ((dev->header_type & 0x7F) == PCI_HEADER_TYPE_NORMAL) {
        dev->subsystem_vendor = pci_read_config16(bus, slot, func, PCI_SUBSYSTEM_VENDOR_ID);
        dev->subsystem_id = pci_read_config16(bus, slot, func, PCI_SUBSYSTEM_ID);
    }
    
    pci_read_bars(dev);
    
    return dev;
}

static void pci_scan_bus(u8 bus);

static void pci_scan_slot(u8 bus, u8 slot) {
    pci_device_t *dev = pci_probe_device(bus, slot, 0);
    if (!dev) return;
    
    // Add to list
    dev->next = pci_devices;
    pci_devices = dev;
    pci_device_count++;
    
    // Check for multifunction device
    if (dev->header_type & PCI_HEADER_MULTIFUNCTION) {
        for (u8 func = 1; func < 8; func++) {
            pci_device_t *func_dev = pci_probe_device(bus, slot, func);
            if (func_dev) {
                func_dev->next = pci_devices;
                pci_devices = func_dev;
                pci_device_count++;
            }
        }
    }
    
    // If this is a PCI-to-PCI bridge, scan the secondary bus
    if (dev->class_code == PCI_CLASS_BRIDGE && dev->subclass == 0x04) {
        u8 secondary_bus = pci_read_config8(bus, slot, 0, 0x19);
        if (secondary_bus != 0) {
            pci_scan_bus(secondary_bus);
        }
    }
}

static void pci_scan_bus(u8 bus) {
    for (u8 slot = 0; slot < 32; slot++) {
        pci_scan_slot(bus, slot);
    }
}

// ============================================================================
// CLASS NAME LOOKUP
// ============================================================================

static const char *pci_class_names[] = {
    [0x00] = "Unclassified",
    [0x01] = "Storage",
    [0x02] = "Network",
    [0x03] = "Display",
    [0x04] = "Multimedia",
    [0x05] = "Memory",
    [0x06] = "Bridge",
    [0x07] = "Communication",
    [0x08] = "System",
    [0x09] = "Input",
    [0x0A] = "Docking",
    [0x0B] = "Processor",
    [0x0C] = "Serial Bus",
    [0x0D] = "Wireless",
    [0x0E] = "Intelligent",
    [0x0F] = "Satellite",
    [0x10] = "Encryption",
    [0x11] = "Signal Processing",
    [0x12] = "Accelerator",
    [0xFF] = "Unassigned",
};

static const char *pci_get_class_name(u8 class_code) {
    if (class_code < 0x13) {
        return pci_class_names[class_code];
    } else if (class_code == 0xFF) {
        return pci_class_names[0xFF];
    }
    return "Unknown";
}

// ============================================================================
// DRIVER REGISTRATION
// ============================================================================

int pci_register_driver(pci_driver_t *driver) {
    driver->next = pci_drivers;
    pci_drivers = driver;
    
    // Try to match with existing devices
    for (pci_device_t *dev = pci_devices; dev; dev = dev->next) {
        if (dev->driver) continue;  // Already has driver
        
        const pci_device_id_t *id = driver->id_table;
        while (id && (id->vendor || id->device || id->class_code)) {
            bool match = true;
            
            if (id->vendor != PCI_ID_ANY && id->vendor != dev->vendor_id)
                match = false;
            if (id->device != PCI_ID_ANY && id->device != dev->device_id)
                match = false;
            if (id->subvendor != PCI_ID_ANY && id->subvendor != dev->subsystem_vendor)
                match = false;
            if (id->subdevice != PCI_ID_ANY && id->subdevice != dev->subsystem_id)
                match = false;
            if (id->class_code) {
                u32 dev_class = (dev->class_code << 16) | (dev->subclass << 8) | dev->prog_if;
                if ((dev_class & id->class_mask) != id->class_code)
                    match = false;
            }
            
            if (match) {
                if (driver->probe && driver->probe(dev, id) == 0) {
                    dev->driver = driver;
                    printk(KERN_INFO "PCI: Driver '%s' bound to %02x:%02x.%x\n",
                           driver->name, dev->bus, dev->slot, dev->func);
                }
                break;
            }
            id++;
        }
    }
    
    return 0;
}

void pci_unregister_driver(pci_driver_t *driver) {
    // Remove from list
    pci_driver_t **pp = &pci_drivers;
    while (*pp) {
        if (*pp == driver) {
            *pp = driver->next;
            break;
        }
        pp = &(*pp)->next;
    }
    
    // Unbind from devices
    for (pci_device_t *dev = pci_devices; dev; dev = dev->next) {
        if (dev->driver == driver) {
            if (driver->remove) {
                driver->remove(dev);
            }
            dev->driver = NULL;
            dev->driver_data = NULL;
        }
    }
}

// ============================================================================
// DEVICE ENABLE/DISABLE
// ============================================================================

void pci_enable_device(pci_device_t *dev) {
    u16 cmd = pci_dev_read16(dev, PCI_COMMAND);
    cmd |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    pci_dev_write16(dev, PCI_COMMAND, cmd);
}

void pci_disable_device(pci_device_t *dev) {
    u16 cmd = pci_dev_read16(dev, PCI_COMMAND);
    cmd &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    pci_dev_write16(dev, PCI_COMMAND, cmd);
}

void pci_set_master(pci_device_t *dev) {
    u16 cmd = pci_dev_read16(dev, PCI_COMMAND);
    cmd |= PCI_COMMAND_MASTER;
    pci_dev_write16(dev, PCI_COMMAND, cmd);
}

// ============================================================================
// LOOKUP
// ============================================================================

pci_device_t *pci_get_device(u16 vendor, u16 device, pci_device_t *from) {
    pci_device_t *dev = from ? from->next : pci_devices;
    
    while (dev) {
        if ((vendor == PCI_ID_ANY || dev->vendor_id == vendor) &&
            (device == PCI_ID_ANY || dev->device_id == device)) {
            return dev;
        }
        dev = dev->next;
    }
    
    return NULL;
}

pci_device_t *pci_get_class(u32 class_code, pci_device_t *from) {
    pci_device_t *dev = from ? from->next : pci_devices;
    
    while (dev) {
        u32 dev_class = (dev->class_code << 16) | (dev->subclass << 8) | dev->prog_if;
        if ((dev_class & 0xFFFF00) == (class_code & 0xFFFF00)) {
            return dev;
        }
        dev = dev->next;
    }
    
    return NULL;
}

// ============================================================================
// INIT
// ============================================================================

void pci_init(void) {
    printk(KERN_INFO "PCI: Scanning buses...\n");
    
    // Check if PCI is available
    u32 tmp = pci_read_config32(0, 0, 0, 0);
    if (tmp == 0xFFFFFFFF) {
        printk(KERN_WARNING "PCI: No PCI bus found\n");
        return;
    }
    
    // Check for multiple host bridges
    u8 header_type = pci_read_config8(0, 0, 0, PCI_HEADER_TYPE);
    if (header_type & PCI_HEADER_MULTIFUNCTION) {
        // Multiple host bridges
        for (u8 func = 0; func < 8; func++) {
            u16 vendor = pci_read_config16(0, 0, func, PCI_VENDOR_ID);
            if (vendor != 0xFFFF) {
                pci_scan_bus(func);
            }
        }
    } else {
        // Single host bridge
        pci_scan_bus(0);
    }
    
    // Print found devices
    printk(KERN_INFO "PCI: Found %d devices:\n", pci_device_count);
    
    for (pci_device_t *dev = pci_devices; dev; dev = dev->next) {
        printk(KERN_INFO "  %02x:%02x.%x [%04x:%04x] %s (%02x:%02x)\n",
               dev->bus, dev->slot, dev->func,
               dev->vendor_id, dev->device_id,
               pci_get_class_name(dev->class_code),
               dev->class_code, dev->subclass);
        
        // Print BARs
        for (int i = 0; i < 6; i++) {
            if (dev->bar[i]) {
                printk(KERN_DEBUG "    BAR%d: 0x%lx (%s, %lu bytes)\n",
                       i, dev->bar[i],
                       dev->bar_type[i] ? "I/O" : "MEM",
                       dev->bar_size[i]);
            }
        }
    }
}

// ============================================================================
// SHELL HELPERS
// ============================================================================

int pci_get_device_count(void) {
    return pci_device_count;
}

void pci_list_devices(void) {
    pci_device_t *dev = pci_devices;
    while (dev) {
        printk("  %02x:%02x.%x [%04x:%04x] %s (class %02x:%02x)\n",
               dev->bus, dev->slot, dev->func,
               dev->vendor_id, dev->device_id,
               pci_get_class_name(dev->class_code),
               dev->class_code, dev->subclass);
        dev = dev->next;
    }
}
