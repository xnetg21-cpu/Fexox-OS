#pragma once
#ifndef UNIFIED_DISK_DRIVER_HEADER
#define UNIFIED_DISK_DRIVER_HEADER

#include <stdint.h>

// ============================
// UNIFIED DISK DRIVER HEADER
// ============================

// Include all disk drivers
#include "disk_driver.h"
#include "nvme.h"
#include "usb_msc.h"

// ============================
// EXTERNAL DEVICE POOLS
// ============================

// From disk_driver.c
extern disk_device_t disk_devices[];
extern uint32_t disk_count;

// From nvme.c
extern nvme_device_t nvme_devices[];
extern uint32_t nvme_device_count;

// From usb_msc.c
extern usb_mass_storage_device_t usb_msc_devices[];
extern uint32_t usb_msc_device_count;

// ============================
// UNIFIED DEVICE INTERFACE
// ============================

typedef enum {
    DISK_INTERFACE_UNKNOWN = 0,
    DISK_INTERFACE_IDE,
    DISK_INTERFACE_SATA,
    DISK_INTERFACE_NVME,
    DISK_INTERFACE_USB,
    DISK_INTERFACE_SCSI,
    DISK_INTERFACE_MMC,
} disk_interface_t;

typedef struct {
    uint32_t device_id;
    disk_interface_t interface_type;
    uint32_t controller_id;  // For multi-device controllers
    
    // Device metadata
    char manufacturer[32];
    char model[48];
    char serial_number[20];
    char firmware_version[8];
    
    // Capacity information
    uint64_t total_sectors;
    uint32_t sector_size;
    uint64_t total_size_bytes;
    
    // Performance
    uint32_t max_transfer_size;
    uint32_t read_cache_size;
    uint32_t write_cache_size;
    
    // Features
    uint32_t supports_dma;
    uint32_t supports_trim;
    uint32_t supports_smart;
    uint32_t supports_ncq;
    uint32_t supports_fua;
    
    // State
    uint32_t online;
    uint32_t read_only;
    uint32_t error_count;
    uint64_t last_error_code;
} unified_disk_info_t;

// ============================
// UNIFIED I/O INTERFACE
// ============================

typedef struct {
    uint32_t device_id;
    uint64_t lba;
    uint32_t block_count;
    void *buffer;
    uint32_t timeout_ms;
    
    // Result
    uint32_t status;
    uint32_t bytes_transferred;
} unified_disk_request_t;

// ============================
// INITIALIZATION
// ============================

uint32_t disk_subsystem_init(void);
uint32_t disk_enumerate_all_devices(void);
uint32_t disk_get_device_count(void);

// ============================
// GENERIC DISK OPERATIONS
// ============================

uint32_t disk_read(unified_disk_request_t *request);
uint32_t disk_write(unified_disk_request_t *request);
uint32_t disk_flush(uint32_t device_id);
uint32_t disk_trim(uint32_t device_id, uint64_t lba, uint32_t block_count);

// ============================
// DEVICE INFORMATION
// ============================

uint32_t disk_get_info(uint32_t device_id, unified_disk_info_t *info);
uint32_t disk_get_capacity(uint32_t device_id, uint64_t *sectors, uint32_t *sector_size);
uint32_t disk_check_online(uint32_t device_id);
uint32_t disk_set_readonly(uint32_t device_id, uint32_t readonly);

// ============================
// HEALTH AND MONITORING
// ============================

uint32_t disk_get_error_count(uint32_t device_id);
uint32_t disk_clear_errors(uint32_t device_id);
uint32_t disk_get_smart_status(uint32_t device_id);
uint32_t disk_run_self_test(uint32_t device_id);

// ============================
// PERFORMANCE OPTIMIZATION
// ============================

uint32_t disk_enable_write_cache(uint32_t device_id);
uint32_t disk_disable_write_cache(uint32_t device_id);
uint32_t disk_set_apm_level(uint32_t device_id, uint8_t level);
uint32_t disk_set_power_mode(uint32_t device_id, uint32_t mode);

// ============================
// INTERNAL HELPERS
// ============================

uint32_t disk_route_read(unified_disk_request_t *request);
uint32_t disk_route_write(unified_disk_request_t *request);
disk_interface_t disk_get_interface_type(uint32_t device_id);
void* disk_get_device_context(uint32_t device_id, disk_interface_t interface);

#endif // UNIFIED_DISK_DRIVER_HEADER
