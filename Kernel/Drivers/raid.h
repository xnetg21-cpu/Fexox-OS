#pragma once

#include <stdint.h>

// ============================
// HARDWARE RAID CONTROLLER
// ============================

#define MAX_RAID_CONTROLLERS 8
#define MAX_RAID_ARRAYS 64
#define MAX_DRIVES_PER_ARRAY 16

// RAID levels
typedef enum {
    RAID_LEVEL_0 = 0,   // Striping
    RAID_LEVEL_1 = 1,   // Mirroring
    RAID_LEVEL_5 = 5,   // Striping with parity
    RAID_LEVEL_6 = 6,   // Dual parity
    RAID_LEVEL_10 = 10, // Mirrored stripe
} raid_level_t;

// Array state
typedef enum {
    RAID_STATE_NORMAL = 0,
    RAID_STATE_DEGRADED = 1,
    RAID_STATE_REBUILDING = 2,
    RAID_STATE_FAILED = 3,
    RAID_STATE_INITIALIZING = 4,
} raid_state_t;

// Drive state
typedef enum {
    RAID_DRIVE_ONLINE = 0,
    RAID_DRIVE_OFFLINE = 1,
    RAID_DRIVE_REBUILDING = 2,
    RAID_DRIVE_FAILED = 3,
} raid_drive_state_t;

// ============================
// RAID STRUCTURES
// ============================

typedef struct {
    uint32_t disk_id;
    raid_drive_state_t state;
    uint64_t rebuild_position;
    uint32_t error_count;
} raid_physical_drive_t;

typedef struct {
    uint32_t array_id;
    raid_level_t level;
    raid_state_t state;
    
    uint32_t stripe_size;  // In sectors (4KB chunks)
    uint32_t stripe_order;
    
    uint32_t drive_count;
    raid_physical_drive_t drives[MAX_DRIVES_PER_ARRAY];
    
    // Capacity
    uint64_t total_sectors;
    uint64_t available_sectors;
    
    // RAID 5/6 specific
    uint32_t parity_drives;
    
    // Rebuild
    uint64_t rebuild_progress;
    uint32_t rebuild_rate_blocks_per_sec;
    
    // Statistics
    uint64_t reads;
    uint64_t writes;
    uint64_t errors;
    
    // Initialization
    uint32_t initialized;
} raid_array_t;

typedef struct {
    uint32_t controller_id;
    
    // PCI info
    uint16_t pci_vendor_id;
    uint16_t pci_device_id;
    uint16_t pci_bus;
    uint16_t pci_device;
    uint8_t pci_function;
    
    // Memory-mapped interface
    volatile void *mmio_base;
    uint32_t mmio_size;
    
    // Status registers
    volatile uint32_t *status_reg;
    volatile uint32_t *control_reg;
    volatile uint32_t *interrupt_status_reg;
    volatile uint32_t *interrupt_mask_reg;
    
    // Arrays
    uint32_t array_count;
    raid_array_t arrays[MAX_RAID_ARRAYS];
    
    // Capabilities
    uint32_t supports_raid0;
    uint32_t supports_raid1;
    uint32_t supports_raid5;
    uint32_t supports_raid6;
    uint32_t supports_raid10;
    uint32_t supports_hot_spare;
    
    // Statistics
    uint64_t total_read_ops;
    uint64_t total_write_ops;
    uint64_t total_errors;
    
    // Timeout
    uint32_t command_timeout_ms;
} raid_controller_t;

// ============================
// RAID OPERATIONS
// ============================

// Controller management
uint32_t raid_init(void);
uint32_t raid_probe_controller(uint16_t bus, uint16_t device, uint8_t function);

// Array management
uint32_t raid_create_array(uint32_t controller_id, raid_level_t level,
                          uint32_t *disk_ids, uint32_t disk_count,
                          uint32_t stripe_size_kb);
uint32_t raid_delete_array(uint32_t controller_id, uint32_t array_id);
uint32_t raid_start_rebuild(uint32_t controller_id, uint32_t array_id);
uint32_t raid_cancel_rebuild(uint32_t controller_id, uint32_t array_id);

// Drive management
uint32_t raid_add_spare_drive(uint32_t controller_id, uint32_t array_id, uint32_t disk_id);
uint32_t raid_remove_drive(uint32_t controller_id, uint32_t array_id, uint32_t drive_index);
uint32_t raid_set_drive_online(uint32_t controller_id, uint32_t array_id, uint32_t drive_index);

// I/O operations
uint32_t raid_read_sectors(uint32_t controller_id, uint32_t array_id,
                          uint64_t lba, uint32_t count, void *buffer);
uint32_t raid_write_sectors(uint32_t controller_id, uint32_t array_id,
                           uint64_t lba, uint32_t count, void *buffer);
uint32_t raid_flush_cache(uint32_t controller_id, uint32_t array_id);

// Array status
uint32_t raid_get_array_info(uint32_t controller_id, uint32_t array_id, raid_array_t *info);
uint32_t raid_get_array_status(uint32_t controller_id, uint32_t array_id);
uint32_t raid_get_rebuild_progress(uint32_t controller_id, uint32_t array_id, uint32_t *percent);

// Statistics
uint32_t raid_get_stats(uint32_t controller_id, uint32_t array_id,
                       uint64_t *reads, uint64_t *writes, uint64_t *errors);

#endif // RAID_CONTROLLER_HEADER
