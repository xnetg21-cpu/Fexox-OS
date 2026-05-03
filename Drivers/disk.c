#include "disk.h"
#include <string.h>

// ============================
// DISK CACHE SYSTEM
// ============================

#define DISK_CACHE_ENTRIES 512
#define DISK_CACHE_HASH_SIZE 256
#define DISK_REQUEST_QUEUE_SIZE 1024
#define DISK_STATISTICS_SAMPLES 1000

typedef struct {
    uint64_t lba;
    uint32_t size;
    uint8_t *data;
    uint32_t flags;
    uint64_t timestamp;
    uint32_t hit_count;
    uint32_t device_id;
    uint32_t interface_type;
} disk_cache_entry_t;

typedef struct {
    uint64_t start_lba;
    uint32_t size;
    uint32_t device_id;
    void *buffer;
    uint32_t priority;
    uint32_t status;
    uint64_t submit_time;
} disk_request_t;

typedef struct {
    uint64_t total_read_sectors;
    uint64_t total_write_sectors;
    uint64_t total_errors;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t avg_read_latency_us;
    uint64_t avg_write_latency_us;
    uint64_t max_read_latency_us;
    uint64_t max_write_latency_us;
} disk_statistics_t;

// ============================
// GLOBAL STATE
// ============================

#define MAX_DISK_DEVICES_TOTAL 64

typedef struct {
    uint32_t device_id;
    disk_interface_t interface_type;
    uint32_t interface_device_id;
    unified_disk_info_t info;
    
    // Cache
    disk_cache_entry_t *cache_entries;
    uint32_t cache_size;
    uint32_t cache_head;
    
    // Performance
    disk_statistics_t stats;
    uint32_t total_requests;
    uint32_t pending_requests;
    
    // Configuration
    uint32_t enable_write_cache;
    uint32_t enable_read_ahead;
    uint32_t read_ahead_size;
    uint32_t max_transfer_size;
    uint32_t command_timeout_ms;
    
    // State
    uint32_t online;
    uint32_t read_only;
    uint32_t power_mode;
    uint32_t error_recovery_mode;
} disk_device_context_t;

static disk_device_context_t disk_registry[MAX_DISK_DEVICES_TOTAL];
static uint32_t disk_registry_count = 0;
static uint32_t disk_registry_lock = 0;

static disk_request_t request_queue[DISK_REQUEST_QUEUE_SIZE];
static uint32_t request_queue_head = 0;
static uint32_t request_queue_tail = 0;
static uint32_t request_queue_lock = 0;

static disk_cache_entry_t cache_pool[DISK_CACHE_ENTRIES];
static uint32_t cache_pool_index = 0;
static uint32_t cache_pool_lock = 0;

// ============================
// REGISTRY MANAGEMENT
// ============================

static void acquire_registry_lock(void) {
    while (__sync_lock_test_and_set(&disk_registry_lock, 1)) {
        __asm__ volatile ("pause");
    }
}

static void release_registry_lock(void) {
    __sync_lock_release(&disk_registry_lock);
}

static uint32_t disk_register_device(disk_interface_t interface, uint32_t interface_device_id) {
    acquire_registry_lock();
    
    if (disk_registry_count >= MAX_DISK_DEVICES_TOTAL) {
        release_registry_lock();
        return 0xFFFFFFFF;
    }
    
    disk_device_context_t *entry = &disk_registry[disk_registry_count];
    entry->device_id = disk_registry_count;
    entry->interface_type = interface;
    entry->interface_device_id = interface_device_id;
    
    uint32_t device_id = disk_registry_count;
    disk_registry_count++;
    
    release_registry_lock();
    return device_id;
}

// ============================
// INITIALIZATION
// ============================

uint32_t disk_subsystem_init(void) {
    // Initialize all disk driver subsystems
    
    // Initialize traditional disk driver (IDE, SATA)
    if (disk_init() != 0) {
        return 1;
    }
    
    // Initialize NVME driver
    if (nvme_init() != 0) {
        return 2;
    }
    
    // Initialize USB mass storage driver
    if (usb_msc_init() != 0) {
        return 3;
    }
    
    return disk_enumerate_all_devices();
}

uint32_t disk_enumerate_all_devices(void) {
    memset(disk_registry, 0, sizeof(disk_registry));
    disk_registry_count = 0;
    
    // Register all IDE/SATA devices
    for (uint32_t i = 0; i < disk_count; i++) {
        disk_register_device(DISK_INTERFACE_IDE, i);
    }
    
    // Register all NVME devices
    for (uint32_t i = 0; i < nvme_device_count; i++) {
        disk_register_device(DISK_INTERFACE_NVME, i);
    }
    
    // Register all USB mass storage devices
    for (uint32_t i = 0; i < usb_msc_device_count; i++) {
        disk_register_device(DISK_INTERFACE_USB, i);
    }
    
    return disk_registry_count;
}

uint32_t disk_get_device_count(void) {
    return disk_registry_count;
}

// ============================
// REQUEST ROUTING
// ============================

disk_interface_t disk_get_interface_type(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return DISK_INTERFACE_UNKNOWN;
    }
    
    return disk_registry[device_id].interface_type;
}

void* disk_get_device_context(uint32_t device_id, disk_interface_t interface) {
    if (device_id >= disk_registry_count) {
        return NULL;
    }
    
    disk_device_context_t *entry = &disk_registry[device_id];
    
    if (entry->interface_type != interface) {
        return NULL;
    }
    
    switch (interface) {
        case DISK_INTERFACE_IDE:
            if (entry->interface_device_id < disk_count) {
                return &disk_devices[entry->interface_device_id];
            }
            break;
        case DISK_INTERFACE_NVME:
            if (entry->interface_device_id < nvme_device_count) {
                return &nvme_devices[entry->interface_device_id];
            }
            break;
        case DISK_INTERFACE_USB:
            if (entry->interface_device_id < usb_msc_device_count) {
                return &usb_msc_devices[entry->interface_device_id];
            }
            break;
        default:
            break;
    }
    
    return NULL;
}

// ============================
// I/O OPERATIONS
// ============================

uint32_t disk_route_read(unified_disk_request_t *request) {
    if (request->device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[request->device_id];
    
    switch (entry->interface_type) {
        case DISK_INTERFACE_IDE:
        case DISK_INTERFACE_SATA:
            return disk_read_sectors(entry->interface_device_id, request->lba, 
                                    request->block_count, request->buffer);
        
        case DISK_INTERFACE_NVME:
            return nvme_read_sectors(entry->interface_device_id, request->lba,
                                    request->block_count, request->buffer);
        
        case DISK_INTERFACE_USB:
            return usb_msc_read_10(&usb_msc_devices[entry->interface_device_id],
                                  request->lba, request->block_count, request->buffer);
        
        default:
            return 2;
    }
}

uint32_t disk_route_write(unified_disk_request_t *request) {
    if (request->device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[request->device_id];
    
    switch (entry->interface_type) {
        case DISK_INTERFACE_IDE:
        case DISK_INTERFACE_SATA:
            return disk_write_sectors(entry->interface_device_id, request->lba,
                                     request->block_count, request->buffer);
        
        case DISK_INTERFACE_NVME:
            return nvme_write_sectors(entry->interface_device_id, request->lba,
                                     request->block_count, request->buffer);
        
        case DISK_INTERFACE_USB:
            return usb_msc_write_10(&usb_msc_devices[entry->interface_device_id],
                                   request->lba, request->block_count, request->buffer);
        
        default:
            return 2;
    }
}

uint32_t disk_read(unified_disk_request_t *request) {
    if (!request || !request->buffer) {
        return 1;
    }
    
    request->status = disk_route_read(request);
    if (request->status == 0) {
        request->bytes_transferred = request->block_count * 512;
    }
    
    return request->status;
}

uint32_t disk_write(unified_disk_request_t *request) {
    if (!request || !request->buffer) {
        return 1;
    }
    
    request->status = disk_route_write(request);
    if (request->status == 0) {
        request->bytes_transferred = request->block_count * 512;
    }
    
    return request->status;
}

uint32_t disk_flush(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[device_id];
    
    switch (entry->interface_type) {
        case DISK_INTERFACE_IDE:
        case DISK_INTERFACE_SATA:
            return disk_flush_cache(entry->interface_device_id);
        
        case DISK_INTERFACE_NVME:
            return nvme_flush_cache(entry->interface_device_id);
        
        case DISK_INTERFACE_USB:
            return usb_msc_synchronize_cache(&usb_msc_devices[entry->interface_device_id]);
        
        default:
            return 2;
    }
}

uint32_t disk_trim(uint32_t device_id, uint64_t lba, uint32_t block_count) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[device_id];
    
    switch (entry->interface_type) {
        case DISK_INTERFACE_IDE:
        case DISK_INTERFACE_SATA:
            return disk_trim(entry->interface_device_id, lba, block_count);
        
        case DISK_INTERFACE_NVME:
            // NVME has DATASET_MGMT command for trim
            return 0;
        
        default:
            return 2;
    }
}

// ============================
// DEVICE INFORMATION
// ============================

uint32_t disk_get_info(uint32_t device_id, unified_disk_info_t *info) {
    if (!info || device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[device_id];
    memcpy(info, &entry->info, sizeof(unified_disk_info_t));
    
    return 0;
}

uint32_t disk_get_capacity(uint32_t device_id, uint64_t *sectors, uint32_t *sector_size) {
    if (!sectors || !sector_size || device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[device_id];
    *sectors = entry->info.total_sectors;
    *sector_size = entry->info.sector_size;
    
    return 0;
}

uint32_t disk_check_online(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    return disk_registry[device_id].info.online ? 0 : 2;
}

uint32_t disk_set_readonly(uint32_t device_id, uint32_t readonly) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_registry[device_id].info.read_only = readonly;
    return 0;
}

// ============================
// HEALTH AND MONITORING
// ============================

uint32_t disk_get_error_count(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return 0xFFFFFFFF;
    }
    
    return disk_registry[device_id].info.error_count;
}

uint32_t disk_clear_errors(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_registry[device_id].info.error_count = 0;
    disk_registry[device_id].info.last_error_code = 0;
    
    return 0;
}

uint32_t disk_get_smart_status(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[device_id];
    
    if (!entry->info.supports_smart) {
        return 2;  // SMART not supported
    }
    
    // TODO: Query SMART status from underlying device
    return 0;
}

uint32_t disk_run_self_test(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[device_id];
    
    if (!entry->info.supports_smart) {
        return 2;  // Self-test not supported
    }
    
    // TODO: Run SMART self-test
    return 0;
}

// ============================
// PERFORMANCE OPTIMIZATION
// ============================

uint32_t disk_enable_write_cache(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    // TODO: Enable write cache in underlying device
    return 0;
}

uint32_t disk_disable_write_cache(uint32_t device_id) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    // Flush cache before disabling
    disk_flush(device_id);
    
    // TODO: Disable write cache in underlying device
    return 0;
}

uint32_t disk_set_apm_level(uint32_t device_id, uint8_t level) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    // TODO: Set APM level in underlying device
    return 0;
}

uint32_t disk_set_power_mode(uint32_t device_id, uint32_t mode) {
    if (device_id >= disk_registry_count) {
        return 1;
    }
    
    disk_device_context_t *entry = &disk_registry[device_id];
    
    switch (entry->interface_type) {
        case DISK_INTERFACE_IDE:
        case DISK_INTERFACE_SATA:
            return disk_set_power_mode(entry->interface_device_id, mode);
        
        default:
            return 2;
    }
}
