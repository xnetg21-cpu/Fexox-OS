#pragma once

#include <stdint.h>
#include <string.h>

// ============================
// DISK DRIVER CONFIGURATION
// ============================

#define MAX_DISK_DEVICES 16
#define MAX_DISK_PARTITIONS 4
#define DISK_SECTOR_SIZE 512
#define DISK_CACHE_BLOCKS 1024
#define DMA_BUFFER_SIZE (256 * 1024)  // 256KB DMA buffer
#define MAX_DISK_OPERATIONS 64
#define DISK_TIMEOUT_MS 5000

// Disk types
#define DISK_TYPE_IDE 0
#define DISK_TYPE_AHCI 1
#define DISK_TYPE_NVME 2

// Disk states
#define DISK_STATE_INIT 0
#define DISK_STATE_READY 1
#define DISK_STATE_BUSY 2
#define DISK_STATE_ERROR 3

// Operation types
#define DISK_OP_READ 0
#define DISK_OP_WRITE 1
#define DISK_OP_TRIM 2

// Partition types
#define PART_TYPE_EMPTY 0x00
#define PART_TYPE_FAT12 0x01
#define PART_TYPE_FAT16 0x04
#define PART_TYPE_EXTENDED 0x05
#define PART_TYPE_FAT32 0x0B
#define PART_TYPE_EXT4 0x83
#define PART_TYPE_EXT_LBA 0x0F

// Cache line flags
#define CACHE_DIRTY 0x01
#define CACHE_VALID 0x02
#define CACHE_LOCKED 0x04

// ============================
// ATA/AHCI DEFINITIONS
// ============================

// ATA Commands
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA 0xCA
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_SEEK 0x70

// ATA Status bits
#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_SRV 0x10
#define ATA_STATUS_DF 0x20
#define ATA_STATUS_RDY 0x40
#define ATA_STATUS_BSY 0x80

// ATA Error bits
#define ATA_ERROR_AMNF 0x01
#define ATA_ERROR_TKZNF 0x02
#define ATA_ERROR_ABRT 0x04
#define ATA_ERROR_MCR 0x08
#define ATA_ERROR_IDNF 0x10
#define ATA_ERROR_MC 0x20
#define ATA_ERROR_UNC 0x40
#define ATA_ERROR_BBK 0x80

// AHCI HBA Memory Registers
#define AHCI_CAP 0x00
#define AHCI_GHC 0x04
#define AHCI_IS 0x08
#define AHCI_PI 0x0C
#define AHCI_VS 0x10
#define AHCI_CCC_CTL 0x14
#define AHCI_CCC_PORTS 0x18
#define AHCI_EM_LOC 0x1C
#define AHCI_EM_CTL 0x20
#define AHCI_CAP2 0x24
#define AHCI_BOHC 0x28

// AHCI Port registers (offset from port base)
#define AHCI_PORT_CLB 0x00      // Command List Base Address
#define AHCI_PORT_CLBU 0x04     // Command List Base Address Upper
#define AHCI_PORT_FB 0x08       // FIS Base Address
#define AHCI_PORT_FBU 0x0C      // FIS Base Address Upper
#define AHCI_PORT_IS 0x10       // Interrupt Status
#define AHCI_PORT_IE 0x14       // Interrupt Enable
#define AHCI_PORT_CMD 0x18      // Command and Status
#define AHCI_PORT_TFD 0x20      // Task File Data
#define AHCI_PORT_SIG 0x24      // Signature
#define AHCI_PORT_SSTS 0x28     // Serial ATA Status
#define AHCI_PORT_SCTL 0x2C     // Serial ATA Control
#define AHCI_PORT_SERR 0x30     // Serial ATA Error
#define AHCI_PORT_SACT 0x34     // Serial ATA Active
#define AHCI_PORT_CI 0x38       // Command Issue
#define AHCI_PORT_SNTF 0x3C     // Serial ATA Notification
#define AHCI_PORT_FBS 0x40      // FIS-based Switching Control

// ============================
// IDE/PATA DEFINITIONS
// ============================

// IDE Port addresses
#define IDE_PRIMARY_CMD 0x1F0
#define IDE_PRIMARY_CTL 0x3F6
#define IDE_SECONDARY_CMD 0x170
#define IDE_SECONDARY_CTL 0x376

// IDE Register offsets
#define IDE_DATA 0x00
#define IDE_ERROR 0x01
#define IDE_SECTOR_COUNT 0x02
#define IDE_SECTOR_NUM 0x03
#define IDE_CYLINDER_LOW 0x04
#define IDE_CYLINDER_HIGH 0x05
#define IDE_HEAD 0x06
#define IDE_STATUS_CMD 0x07
#define IDE_CONTROL 0x06

// ============================
// DATA STRUCTURES
// ============================

typedef struct {
    uint64_t lba;
    uint32_t size;
    uint32_t flags;
} cache_block_t;

typedef struct {
    uint32_t id;
    uint64_t lba_start;
    uint64_t lba_size;
    uint32_t type;
    uint32_t flags;
} disk_partition_t;

typedef struct {
    uint32_t status;
    uint32_t operation;
    uint64_t lba;
    uint32_t sector_count;
    void *buffer;
    uint32_t flags;
    uint64_t timestamp;
} disk_operation_t;

typedef struct {
    uint16_t cylinder;
    uint8_t head;
    uint8_t sector;
} chs_t;

typedef struct {
    uint32_t device_id;
    uint32_t type;  // IDE or AHCI
    uint32_t state;
    uint32_t status;
    
    // Device info
    uint16_t port_num;
    uint16_t bus_num;
    uint16_t device_num;
    uint16_t function_num;
    
    // Geometry
    uint64_t total_sectors;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_track;
    uint32_t heads;
    uint32_t cylinders;
    
    // Capabilities
    uint32_t dma_capable;
    uint32_t lba48_capable;
    uint32_t smart_capable;
    uint32_t ncq_capable;
    
    // Controller addresses
    uint32_t cmd_base;
    uint32_t ctl_base;
    uint32_t ahci_base;
    
    // Buffers
    uint8_t *dma_buffer;
    uint64_t dma_physical_addr;
    
    // Cache
    cache_block_t *cache;
    uint32_t cache_size;
    uint32_t cache_hits;
    uint32_t cache_misses;
    
    // Partitions
    disk_partition_t partitions[MAX_DISK_PARTITIONS];
    uint32_t partition_count;
    
    // Operations queue
    disk_operation_t operations[MAX_DISK_OPERATIONS];
    uint32_t op_head;
    uint32_t op_tail;
    
    // Statistics
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_errors;
    uint64_t last_error_code;
    
} disk_device_t;

// ============================
// AHCI-specific structures
// ============================

typedef struct {
    uint8_t fis_type;
    uint8_t flags;
    uint8_t cmd;
    uint8_t features_low;
    uint64_t lba;
    uint32_t count;
    uint32_t reserved1;
    uint32_t reserved2;
} ahci_cmd_fis_t;

typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t dw4;
    uint32_t dw5;
    uint16_t byte_count;
    uint8_t reserved;
    uint8_t flags;
} ahci_prd_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t atapi_cmd[16];
    uint8_t reserved[48];
    ahci_prd_entry_t prdt[256];
} ahci_cmd_table_t;

typedef struct {
    uint32_t cmd_table_addr_low;
    uint32_t cmd_table_addr_high;
    uint32_t reserved[4];
} ahci_cmd_header_t;

// ============================
// FUNCTION PROTOTYPES
// ============================

// Disk initialization
uint32_t disk_init(void);
uint32_t disk_detect_devices(void);
uint32_t disk_probe_device(uint16_t bus, uint16_t device, uint16_t function);

// Device operations
uint32_t disk_read_sectors(uint32_t disk_id, uint64_t lba, uint32_t count, void *buffer);
uint32_t disk_write_sectors(uint32_t disk_id, uint64_t lba, uint32_t count, void *buffer);
uint32_t disk_flush_cache(uint32_t disk_id);
uint32_t disk_trim(uint32_t disk_id, uint64_t lba, uint32_t count);

// Async operations
uint32_t disk_read_async(uint32_t disk_id, uint64_t lba, uint32_t count, void *buffer);
uint32_t disk_write_async(uint32_t disk_id, uint64_t lba, uint32_t count, void *buffer);
uint32_t disk_wait_operation(uint32_t disk_id, uint32_t op_id);

// Partition management
uint32_t disk_read_mbr(uint32_t disk_id);
uint32_t disk_parse_partitions(uint32_t disk_id);
uint32_t disk_get_partition_info(uint32_t disk_id, uint32_t part_id, disk_partition_t *info);

// Cache operations
uint32_t disk_driver_cache_read(uint32_t disk_id, uint64_t lba, void *buffer);
uint32_t disk_driver_cache_write(uint32_t disk_id, uint64_t lba, void *buffer);
uint32_t disk_driver_cache_invalidate(uint32_t disk_id, uint64_t lba);
uint32_t disk_driver_cache_flush(uint32_t disk_id);

// SMART operations
uint32_t disk_driver_read_smart_data(uint32_t disk_id, void *buffer);
uint32_t disk_driver_get_error_log(uint32_t disk_id, void *buffer);

// Utility functions
uint32_t disk_get_device_info(uint32_t disk_id, disk_device_t *info);
uint32_t disk_driver_reset_device(uint32_t disk_id);
uint32_t disk_driver_self_test(uint32_t disk_id);
uint32_t disk_driver_set_power_mode(uint32_t disk_id, uint32_t mode);

// IRQ handling
void disk_irq_handler(uint32_t irq);
void disk_irq_register(uint32_t disk_id, uint32_t irq);

// Low-level operations
uint32_t ahci_send_command(disk_device_t *disk, uint8_t cmd, uint64_t lba, uint32_t count, void *buffer);
uint32_t ide_send_command(disk_device_t *disk, uint8_t cmd, uint64_t lba, uint32_t count, void *buffer);
uint32_t disk_wait_busy(disk_device_t *disk, uint32_t timeout_ms);

// Statistics
uint32_t disk_get_stats(uint32_t disk_id, uint64_t *reads, uint64_t *writes, uint64_t *errors);
void disk_print_stats(uint32_t disk_id);
