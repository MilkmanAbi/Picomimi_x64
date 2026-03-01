/**
 * Picomimi-x64 Intel e1000 Network Driver
 * 
 * Basic driver for Intel 82540EM (e1000) Gigabit Ethernet
 */

#include <kernel/types.h>
#include <drivers/pci.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/io.h>

// ============================================================================
// E1000 REGISTERS
// ============================================================================

// Control registers
#define E1000_CTRL      0x0000  // Device Control
#define E1000_STATUS    0x0008  // Device Status
#define E1000_EECD      0x0010  // EEPROM/Flash Control
#define E1000_EERD      0x0014  // EEPROM Read
#define E1000_CTRL_EXT  0x0018  // Extended Device Control
#define E1000_FLA       0x001C  // Flash Access
#define E1000_MDIC      0x0020  // MDI Control
#define E1000_FCAL      0x0028  // Flow Control Address Low
#define E1000_FCAH      0x002C  // Flow Control Address High
#define E1000_FCT       0x0030  // Flow Control Type
#define E1000_VET       0x0038  // VLAN Ether Type
#define E1000_FCTTV     0x0170  // Flow Control Transmit Timer Value
#define E1000_TXCW      0x0178  // Transmit Configuration Word
#define E1000_RXCW      0x0180  // Receive Configuration Word
#define E1000_LEDCTL    0x0E00  // LED Control

// Interrupt registers
#define E1000_ICR       0x00C0  // Interrupt Cause Read
#define E1000_ICS       0x00C8  // Interrupt Cause Set
#define E1000_IMS       0x00D0  // Interrupt Mask Set
#define E1000_IMC       0x00D8  // Interrupt Mask Clear

// Receive registers
#define E1000_RCTL      0x0100  // Receive Control
#define E1000_RDBAL     0x2800  // RX Descriptor Base Address Low
#define E1000_RDBAH     0x2804  // RX Descriptor Base Address High
#define E1000_RDLEN     0x2808  // RX Descriptor Length
#define E1000_RDH       0x2810  // RX Descriptor Head
#define E1000_RDT       0x2818  // RX Descriptor Tail
#define E1000_RDTR      0x2820  // RX Delay Timer
#define E1000_RADV      0x282C  // RX Interrupt Absolute Delay
#define E1000_RAL       0x5400  // Receive Address Low
#define E1000_RAH       0x5404  // Receive Address High

// Transmit registers
#define E1000_TCTL      0x0400  // Transmit Control
#define E1000_TIPG      0x0410  // Transmit Inter-Packet Gap
#define E1000_TDBAL     0x3800  // TX Descriptor Base Address Low
#define E1000_TDBAH     0x3804  // TX Descriptor Base Address High
#define E1000_TDLEN     0x3808  // TX Descriptor Length
#define E1000_TDH       0x3810  // TX Descriptor Head
#define E1000_TDT       0x3818  // TX Descriptor Tail

// Statistics registers
#define E1000_CRCERRS   0x4000  // CRC Error Count
#define E1000_ALGNERRC  0x4004  // Alignment Error Count
#define E1000_MPC       0x4010  // Missed Packets Count
#define E1000_GPTC      0x4080  // Good Packets Transmitted Count
#define E1000_GPRC      0x4074  // Good Packets Received Count

// CTRL register bits
#define E1000_CTRL_FD       0x00000001  // Full Duplex
#define E1000_CTRL_GIO_DIS  0x00000004  // GIO Master Disable
#define E1000_CTRL_LRST     0x00000008  // Link Reset
#define E1000_CTRL_ASDE     0x00000020  // Auto-Speed Detection Enable
#define E1000_CTRL_SLU      0x00000040  // Set Link Up
#define E1000_CTRL_ILOS     0x00000080  // Invert Loss-of-Signal
#define E1000_CTRL_RST      0x04000000  // Device Reset
#define E1000_CTRL_VME      0x40000000  // VLAN Mode Enable
#define E1000_CTRL_PHY_RST  0x80000000  // PHY Reset

// RCTL register bits
#define E1000_RCTL_EN       0x00000002  // Receiver Enable
#define E1000_RCTL_SBP      0x00000004  // Store Bad Packets
#define E1000_RCTL_UPE      0x00000008  // Unicast Promiscuous Enable
#define E1000_RCTL_MPE      0x00000010  // Multicast Promiscuous Enable
#define E1000_RCTL_LPE      0x00000020  // Long Packet Enable
#define E1000_RCTL_LBM_MAC  0x00000040  // Loopback Mode - MAC
#define E1000_RCTL_RDMTS_HALF 0x00000000 // RX Desc Min Threshold 1/2
#define E1000_RCTL_BAM      0x00008000  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2048 0x00000000 // Buffer Size 2048
#define E1000_RCTL_BSIZE_1024 0x00010000 // Buffer Size 1024
#define E1000_RCTL_SECRC    0x04000000  // Strip Ethernet CRC

// TCTL register bits
#define E1000_TCTL_EN       0x00000002  // Transmitter Enable
#define E1000_TCTL_PSP      0x00000008  // Pad Short Packets
#define E1000_TCTL_CT       0x00000FF0  // Collision Threshold
#define E1000_TCTL_COLD     0x003FF000  // Collision Distance
#define E1000_TCTL_RTLC     0x01000000  // Retransmit on Late Collision

// Interrupt bits
#define E1000_ICR_TXDW      0x00000001  // Transmit Descriptor Written Back
#define E1000_ICR_TXQE      0x00000002  // Transmit Queue Empty
#define E1000_ICR_LSC       0x00000004  // Link Status Change
#define E1000_ICR_RXSEQ     0x00000008  // RX Sequence Error
#define E1000_ICR_RXDMT0    0x00000010  // RX Descriptor Minimum Threshold
#define E1000_ICR_RXO       0x00000040  // RX Overrun
#define E1000_ICR_RXT0      0x00000080  // RX Timer Interrupt

// ============================================================================
// DESCRIPTORS
// ============================================================================

#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32
#define E1000_RX_BUFFER_SIZE 2048

// Receive Descriptor
typedef struct {
    u64 addr;       // Buffer address
    u16 length;     // Length
    u16 checksum;   // Checksum
    u8  status;     // Status
    u8  errors;     // Errors
    u16 special;    // Special
} __packed e1000_rx_desc_t;

// Transmit Descriptor
typedef struct {
    u64 addr;       // Buffer address
    u16 length;     // Length
    u8  cso;        // Checksum Offset
    u8  cmd;        // Command
    u8  sta;        // Status
    u8  css;        // Checksum Start
    u16 special;    // Special
} __packed e1000_tx_desc_t;

// RX status bits
#define E1000_RXD_STAT_DD   0x01    // Descriptor Done
#define E1000_RXD_STAT_EOP  0x02    // End of Packet

// TX command bits
#define E1000_TXD_CMD_EOP   0x01    // End of Packet
#define E1000_TXD_CMD_IFCS  0x02    // Insert FCS
#define E1000_TXD_CMD_RS    0x08    // Report Status

// TX status bits
#define E1000_TXD_STAT_DD   0x01    // Descriptor Done

// ============================================================================
// DRIVER STATE
// ============================================================================

typedef struct e1000_device {
    pci_device_t    *pci_dev;
    
    // MMIO base
    volatile u32    *mmio_base;
    u64             mmio_size;
    
    // I/O port base (if used)
    u16             io_base;
    
    // MAC address
    u8              mac_addr[6];
    
    // Descriptors
    e1000_rx_desc_t *rx_descs;
    e1000_tx_desc_t *tx_descs;
    phys_addr_t     rx_descs_phys;
    phys_addr_t     tx_descs_phys;
    
    // Buffers
    void            *rx_buffers[E1000_NUM_RX_DESC];
    void            *tx_buffers[E1000_NUM_TX_DESC];
    
    // Indices
    u32             rx_cur;
    u32             tx_cur;
    
    // IRQ
    u8              irq;
    
    // Statistics
    u64             rx_packets;
    u64             tx_packets;
    u64             rx_bytes;
    u64             tx_bytes;
    
    struct e1000_device *next;
} e1000_device_t;

static e1000_device_t *e1000_devices = NULL;

// ============================================================================
// MMIO ACCESS
// ============================================================================

static inline void e1000_write32(e1000_device_t *dev, u32 reg, u32 value) {
    dev->mmio_base[reg / 4] = value;
    __asm__ volatile("" ::: "memory");  // Memory barrier
}

static inline u32 e1000_read32(e1000_device_t *dev, u32 reg) {
    __asm__ volatile("" ::: "memory");  // Memory barrier
    return dev->mmio_base[reg / 4];
}

// ============================================================================
// EEPROM ACCESS
// ============================================================================

static u16 e1000_eeprom_read(e1000_device_t *dev, u8 addr) {
    u32 val;
    
    e1000_write32(dev, E1000_EERD, (1) | ((u32)addr << 8));
    
    // Wait for done bit
    for (int i = 0; i < 1000; i++) {
        val = e1000_read32(dev, E1000_EERD);
        if (val & (1 << 4)) {
            return (u16)(val >> 16);
        }
    }
    
    return 0;
}

// ============================================================================
// MAC ADDRESS
// ============================================================================

static void e1000_read_mac(e1000_device_t *dev) {
    // Try reading from EEPROM first
    u16 mac0 = e1000_eeprom_read(dev, 0);
    u16 mac1 = e1000_eeprom_read(dev, 1);
    u16 mac2 = e1000_eeprom_read(dev, 2);
    
    if (mac0 != 0 || mac1 != 0 || mac2 != 0) {
        dev->mac_addr[0] = mac0 & 0xFF;
        dev->mac_addr[1] = mac0 >> 8;
        dev->mac_addr[2] = mac1 & 0xFF;
        dev->mac_addr[3] = mac1 >> 8;
        dev->mac_addr[4] = mac2 & 0xFF;
        dev->mac_addr[5] = mac2 >> 8;
        return;
    }
    
    // Read from RAL/RAH registers
    u32 ral = e1000_read32(dev, E1000_RAL);
    u32 rah = e1000_read32(dev, E1000_RAH);
    
    dev->mac_addr[0] = ral & 0xFF;
    dev->mac_addr[1] = (ral >> 8) & 0xFF;
    dev->mac_addr[2] = (ral >> 16) & 0xFF;
    dev->mac_addr[3] = (ral >> 24) & 0xFF;
    dev->mac_addr[4] = rah & 0xFF;
    dev->mac_addr[5] = (rah >> 8) & 0xFF;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

static int e1000_reset(e1000_device_t *dev) {
    // Disable interrupts
    e1000_write32(dev, E1000_IMC, 0xFFFFFFFF);
    e1000_read32(dev, E1000_ICR);  // Clear pending
    
    // Reset the device
    u32 ctrl = e1000_read32(dev, E1000_CTRL);
    e1000_write32(dev, E1000_CTRL, ctrl | E1000_CTRL_RST);
    
    // Wait for reset to complete
    for (int i = 0; i < 1000; i++) {
        if (!(e1000_read32(dev, E1000_CTRL) & E1000_CTRL_RST)) {
            break;
        }
        // Small delay
        for (volatile int j = 0; j < 1000; j++);
    }
    
    // Disable interrupts again
    e1000_write32(dev, E1000_IMC, 0xFFFFFFFF);
    e1000_read32(dev, E1000_ICR);
    
    return 0;
}

static int __attribute__((unused)) e1000_init_rx(e1000_device_t *dev) {
    // Allocate RX descriptors
    dev->rx_descs = kmalloc(sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC + 16, GFP_KERNEL);
    if (!dev->rx_descs) return -ENOMEM;
    
    // Align to 16 bytes
    dev->rx_descs = (e1000_rx_desc_t *)(((u64)dev->rx_descs + 15) & ~15);
    memset(dev->rx_descs, 0, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    
    // TODO: Get physical address properly
    dev->rx_descs_phys = (phys_addr_t)dev->rx_descs;  // Simplified
    
    // Allocate RX buffers
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        dev->rx_buffers[i] = kmalloc(E1000_RX_BUFFER_SIZE + 16, GFP_KERNEL);
        if (!dev->rx_buffers[i]) return -ENOMEM;
        
        dev->rx_descs[i].addr = (u64)dev->rx_buffers[i];  // Simplified
        dev->rx_descs[i].status = 0;
    }
    
    // Setup RX descriptor ring
    e1000_write32(dev, E1000_RDBAL, (u32)(dev->rx_descs_phys & 0xFFFFFFFF));
    e1000_write32(dev, E1000_RDBAH, (u32)(dev->rx_descs_phys >> 32));
    e1000_write32(dev, E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write32(dev, E1000_RDH, 0);
    e1000_write32(dev, E1000_RDT, E1000_NUM_RX_DESC - 1);
    
    dev->rx_cur = 0;
    
    // Enable receiver
    u32 rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC;
    e1000_write32(dev, E1000_RCTL, rctl);
    
    return 0;
}

static int __attribute__((unused)) e1000_init_tx(e1000_device_t *dev) {
    // Allocate TX descriptors
    dev->tx_descs = kmalloc(sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC + 16, GFP_KERNEL);
    if (!dev->tx_descs) return -ENOMEM;
    
    // Align to 16 bytes
    dev->tx_descs = (e1000_tx_desc_t *)(((u64)dev->tx_descs + 15) & ~15);
    memset(dev->tx_descs, 0, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    
    dev->tx_descs_phys = (phys_addr_t)dev->tx_descs;
    
    // Allocate TX buffers
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        dev->tx_buffers[i] = kmalloc(E1000_RX_BUFFER_SIZE, GFP_KERNEL);
        if (!dev->tx_buffers[i]) return -ENOMEM;
        
        dev->tx_descs[i].addr = (u64)dev->tx_buffers[i];
        dev->tx_descs[i].sta = E1000_TXD_STAT_DD;  // Ready
    }
    
    // Setup TX descriptor ring
    e1000_write32(dev, E1000_TDBAL, (u32)(dev->tx_descs_phys & 0xFFFFFFFF));
    e1000_write32(dev, E1000_TDBAH, (u32)(dev->tx_descs_phys >> 32));
    e1000_write32(dev, E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write32(dev, E1000_TDH, 0);
    e1000_write32(dev, E1000_TDT, 0);
    
    dev->tx_cur = 0;
    
    // Setup TIPG
    e1000_write32(dev, E1000_TIPG, 0x0060200A);
    
    // Enable transmitter
    u32 tctl = E1000_TCTL_EN | E1000_TCTL_PSP | (15 << 4) | (64 << 12) | E1000_TCTL_RTLC;
    e1000_write32(dev, E1000_TCTL, tctl);
    
    return 0;
}

static void __attribute__((unused)) e1000_link_up(e1000_device_t *dev) {
    u32 ctrl = e1000_read32(dev, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    e1000_write32(dev, E1000_CTRL, ctrl);
    
    // Wait a bit for link
    for (volatile int i = 0; i < 100000; i++);
    
    u32 status = e1000_read32(dev, E1000_STATUS);
    if (status & 2) {
        printk(KERN_INFO "e1000: Link up\n");
    } else {
        printk(KERN_WARNING "e1000: Link down\n");
    }
}

// ============================================================================
// PCI DRIVER
// ============================================================================

static int e1000_probe(pci_device_t *pci_dev, const pci_device_id_t *id) {
    (void)id;
    
    printk(KERN_INFO "e1000: Initializing %02x:%02x.%x\n",
           pci_dev->bus, pci_dev->slot, pci_dev->func);
    
    // Enable PCI device
    pci_enable_device(pci_dev);
    pci_set_master(pci_dev);
    
    // Allocate device structure
    e1000_device_t *dev = kmalloc(sizeof(e1000_device_t), GFP_KERNEL);
    if (!dev) return -ENOMEM;
    
    memset(dev, 0, sizeof(*dev));
    dev->pci_dev = pci_dev;
    pci_dev->driver_data = dev;
    
    // Get MMIO and I/O info
    u64 mmio_phys = pci_dev->bar[0];
    dev->mmio_size = pci_dev->bar_size[0];
    dev->io_base = (u16)pci_dev->bar[1];
    dev->irq = pci_dev->interrupt_line;
    
    printk(KERN_INFO "e1000: MMIO phys=0x%lx size=%lu, I/O=0x%x, IRQ %d\n",
           (unsigned long)mmio_phys, (unsigned long)dev->mmio_size, 
           dev->io_base, dev->irq);
    
    // Map MMIO into virtual address space using ioremap
    extern void *ioremap(u64 phys_addr, size_t size);
    dev->mmio_base = (volatile u32 *)ioremap(mmio_phys, dev->mmio_size);
    if (!dev->mmio_base) {
        printk(KERN_ERR "e1000: Failed to map MMIO\n");
        kfree(dev);
        return -ENOMEM;
    }
    
    printk(KERN_INFO "e1000: MMIO mapped to 0x%lx\n", (unsigned long)dev->mmio_base);
    
    // Reset device
    e1000_reset(dev);
    
    // Read MAC address
    e1000_read_mac(dev);
    printk(KERN_INFO "e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
           dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);
    
    // Set MAC address in RAL/RAH
    e1000_write32(dev, E1000_RAL, 
                  dev->mac_addr[0] | (dev->mac_addr[1] << 8) |
                  (dev->mac_addr[2] << 16) | (dev->mac_addr[3] << 24));
    e1000_write32(dev, E1000_RAH,
                  dev->mac_addr[4] | (dev->mac_addr[5] << 8) | (1 << 31));
    
    // Add to device list
    dev->next = e1000_devices;
    e1000_devices = dev;
    
    printk(KERN_INFO "e1000: Device initialized successfully\n");
    
    return 0;
}

static void e1000_remove(pci_device_t *pci_dev) {
    e1000_device_t *dev = pci_dev->driver_data;
    if (!dev) return;
    
    // Disable device
    e1000_write32(dev, E1000_RCTL, 0);
    e1000_write32(dev, E1000_TCTL, 0);
    e1000_write32(dev, E1000_IMC, 0xFFFFFFFF);
    
    // Free buffers
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        if (dev->rx_buffers[i]) kfree(dev->rx_buffers[i]);
    }
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        if (dev->tx_buffers[i]) kfree(dev->tx_buffers[i]);
    }
    
    kfree(dev);
    pci_dev->driver_data = NULL;
}

// ============================================================================
// DRIVER REGISTRATION
// ============================================================================

static const pci_device_id_t e1000_ids[] = {
    { PCI_DEVICE(0x8086, 0x100E) },  // 82540EM
    { PCI_DEVICE(0x8086, 0x100F) },  // 82545EM
    { PCI_DEVICE(0x8086, 0x10D3) },  // 82574L
    { PCI_DEVICE(0x8086, 0x10EA) },  // 82577LM
    { PCI_DEVICE(0x8086, 0x1502) },  // 82579LM
    { 0 }
};

static pci_driver_t e1000_driver = {
    .name = "e1000",
    .id_table = e1000_ids,
    .probe = e1000_probe,
    .remove = e1000_remove,
};

void e1000_init(void) {
    printk(KERN_INFO "e1000: Registering driver\n");
    pci_register_driver(&e1000_driver);
}
