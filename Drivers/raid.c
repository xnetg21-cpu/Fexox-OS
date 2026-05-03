#include "raid.h"
#include "disk.h"
#include <string.h>

// ============================
// RAID CONTROLLER POOL
// ============================

raid_controller_t raid_controllers[MAX_RAID_CONTROLLERS];
uint32_t raid_controller_count = 0;
uint32_t raid_lock = 0;

// ============================
// SYNCHRONIZATION
// ============================

static void acquire_raid_lock(void) {
    while (__sync_lock_test_and_set(&raid_lock, 1)) {
        __asm__ volatile ("pause");
    }
}

static void release_raid_lock(void) {
    __sync_lock_release(&raid_lock);
}

// ============================
// UTILITY FUNCTIONS
// ============================

static uint32_t raid_get_block_count(raid_array_t *array) {
    switch (array->level) {
        case RAID_LEVEL_0:
            // Stripe all drives
            return array->drive_count;
        
        case RAID_LEVEL_1:
            // Mirror, capacity = one drive
            return 1;
        
        case RAID_LEVEL_5:
            // N-1 drives worth of space (one parity)
            return array->drive_count - 1;
        
        case RAID_LEVEL_6:
            // N-2 drives worth of space (two parity)
            return array->drive_count - 2;
        
        case RAID_LEVEL_10:
            // (N/2) drives worth of space (mirrored pairs)
            return array->drive_count / 2;
        
        default:
            return 0;
    }
}

static uint32_t raid_calculate_stripe_index(raid_array_t *array, uint64_t lba) {
    uint64_t stripe_number = lba >> array->stripe_order;
    return stripe_number % array->drive_count;
}

static uint64_t raid_calculate_physical_lba(raid_array_t *array, uint64_t logical_lba, uint32_t drive_index) {
    uint64_t stripe_number = logical_lba >> array->stripe_order;
    uint64_t stripe_offset = logical_lba & ((1UL << array->stripe_order) - 1);
    
    switch (array->level) {
        case RAID_LEVEL_0:
            // Linear mapping
            return (stripe_number * array->drive_count + drive_index) << array->stripe_order | stripe_offset;
        
        case RAID_LEVEL_5:
            {
                uint32_t data_drives = array->drive_count - 1;
                uint32_t data_index = stripe_number % data_drives;
                uint32_t parity_drive = stripe_number % array->drive_count;
                
                if (drive_index == parity_drive) {
                    return (stripe_number << array->stripe_order) | stripe_offset;
                }
                
                uint32_t adjusted_index = (drive_index < parity_drive) ? drive_index : (drive_index - 1);
                return ((stripe_number / data_drives) * array->drive_count + adjusted_index + 1) << array->stripe_order | stripe_offset;
            }
        
        default:
            return logical_lba;
    }
}

// ============================
// RAID 0 - STRIPING
// ============================

static uint32_t raid0_read(raid_array_t *array, uint64_t lba, uint32_t count, void *buffer) {
    uint32_t stripe_index = raid_calculate_stripe_index(array, lba);
    uint64_t physical_lba = raid_calculate_physical_lba(array, lba, stripe_index);
    
    uint32_t disk_id = array->drives[stripe_index].disk_id;
    
    return disk_read_sectors(disk_id, physical_lba, count, buffer);
}

static uint32_t raid0_write(raid_array_t *array, uint64_t lba, uint32_t count, void *buffer) {
    uint32_t stripe_index = raid_calculate_stripe_index(array, lba);
    uint64_t physical_lba = raid_calculate_physical_lba(array, lba, stripe_index);
    
    uint32_t disk_id = array->drives[stripe_index].disk_id;
    
    return disk_write_sectors(disk_id, physical_lba, count, buffer);
}

// ============================
// RAID 1 - MIRRORING
// ============================

static uint32_t raid1_read(raid_array_t *array, uint64_t lba, uint32_t count, void *buffer) {
    // Read from first online drive
    for (uint32_t i = 0; i < array->drive_count; i++) {
        if (array->drives[i].state == RAID_DRIVE_ONLINE) {
            return disk_read_sectors(array->drives[i].disk_id, lba, count, buffer);
        }
    }
    
    return 1;  // No online drives
}

static uint32_t raid1_write(raid_array_t *array, uint64_t lba, uint32_t count, void *buffer) {
    uint32_t result = 0;
    
    // Write to all online drives
    for (uint32_t i = 0; i < array->drive_count; i++) {
        if (array->drives[i].state == RAID_DRIVE_ONLINE) {
            if (disk_write_sectors(array->drives[i].disk_id, lba, count, buffer) != 0) {
                array->drives[i].state = RAID_DRIVE_FAILED;
                array->drives[i].error_count++;
                result = 2;  // Partial write failure
            }
        }
    }
    
    return result;
}

// ============================
// RAID 5 - STRIPING WITH PARITY
// ============================

static void raid5_calculate_parity(void *data_buffers[], uint32_t stripe_size, void *parity) {
    uint8_t *data[MAX_DRIVES_PER_ARRAY];
    uint8_t *par = (uint8_t *)parity;
    
    // XOR all data drives together for parity
    memset(par, 0, stripe_size);
    
    for (uint32_t i = 0; i < MAX_DRIVES_PER_ARRAY - 1; i++) {
        data[i] = (uint8_t *)data_buffers[i];
        
        for (uint32_t j = 0; j < stripe_size; j++) {
            par[j] ^= data[i][j];
        }
    }
}

static uint32_t raid5_read(raid_array_t *array, uint64_t lba, uint32_t count, void *buffer) {
    uint32_t stripe_index = raid_calculate_stripe_index(array, lba);
    
    // Read from appropriate data drive
    for (uint32_t i = 0; i < array->drive_count; i++) {
        if (i != stripe_index && array->drives[i].state == RAID_DRIVE_ONLINE) {
            uint64_t physical_lba = raid_calculate_physical_lba(array, lba, i);
            return disk_read_sectors(array->drives[i].disk_id, physical_lba, count, buffer);
        }
    }
    
    return 1;  // No available drives
}

static uint32_t raid5_write(raid_array_t *array, uint64_t lba, uint32_t count, void *buffer) {
    uint32_t stripe_index = raid_calculate_stripe_index(array, lba);
    uint32_t result = 0;
    
    // Write data and parity
    for (uint32_t i = 0; i < array->drive_count; i++) {
        if (array->drives[i].state == RAID_DRIVE_ONLINE) {
            uint64_t physical_lba = raid_calculate_physical_lba(array, lba, i);
            if (disk_write_sectors(array->drives[i].disk_id, physical_lba, count, buffer) != 0) {
                array->drives[i].state = RAID_DRIVE_FAILED;
                result = 2;
            }
        }
    }
    
    return result;
}

// ============================
// PUBLIC INTERFACE
// ============================

uint32_t raid_init(void) {
    memset(raid_controllers, 0, sizeof(raid_controllers));
    raid_controller_count = 0;
    
    // TODO: Scan PCI bus for RAID controllers
    
    return 0;
}

uint32_t raid_probe_controller(uint16_t bus, uint16_t device, uint8_t function) {
    if (raid_controller_count >= MAX_RAID_CONTROLLERS) {
        return 1;
    }
    
    raid_controller_t *controller = &raid_controllers[raid_controller_count];
    
    controller->controller_id = raid_controller_count;
    controller->pci_bus = bus;
    controller->pci_device = device;
    controller->pci_function = function;
    
    // TODO: Read PCI configuration
    // controller->pci_vendor_id = pci_read_word(bus, device, function, 0x00);
    // controller->pci_device_id = pci_read_word(bus, device, function, 0x02);
    
    controller->command_timeout_ms = 5000;
    
    raid_controller_count++;
    return controller->controller_id;
}

uint32_t raid_create_array(uint32_t controller_id, raid_level_t level,
                          uint32_t *disk_ids, uint32_t disk_count,
                          uint32_t stripe_size_kb) {
    if (controller_id >= raid_controller_count) {
        return 0xFFFFFFFF;
    }
    
    raid_controller_t *controller = &raid_controllers[controller_id];
    
    if (controller->array_count >= MAX_RAID_ARRAYS) {
        return 0xFFFFFFFF;
    }
    
    acquire_raid_lock();
    
    raid_array_t *array = &controller->arrays[controller->array_count];
    
    array->array_id = controller->array_count;
    array->level = level;
    array->state = RAID_STATE_INITIALIZING;
    array->drive_count = disk_count;
    array->stripe_size = stripe_size_kb * 2;  // Convert KB to sectors
    
    // Calculate stripe order (log2)
    uint32_t size = array->stripe_size;
    array->stripe_order = 0;
    while (size > 1) {
        array->stripe_order++;
        size >>= 1;
    }
    
    // Add drives
    for (uint32_t i = 0; i < disk_count; i++) {
        array->drives[i].disk_id = disk_ids[i];
        array->drives[i].state = RAID_DRIVE_ONLINE;
        array->drives[i].error_count = 0;
    }
    
    // Calculate capacity
    uint32_t effective_drives = raid_get_block_count(array);
    if (effective_drives > 0) {
        // Get drive capacity
        uint64_t drive_sectors = 0;
        unified_disk_info_t info;
        if (disk_get_info(disk_ids[0], &info) == 0) {
            drive_sectors = info.total_sectors;
        }
        
        array->total_sectors = drive_sectors * effective_drives;
        array->available_sectors = array->total_sectors;
    }
    
    array->initialized = 1;
    array->state = RAID_STATE_NORMAL;
    
    uint32_t array_id = controller->array_count;
    controller->array_count++;
    
    release_raid_lock();
    
    return array_id;
}

uint32_t raid_delete_array(uint32_t controller_id, uint32_t array_id) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return 1;
    }
    
    raid_controller_t *controller = &raid_controllers[controller_id];
    
    acquire_raid_lock();
    
    if (array_id < controller->array_count) {
        // Mark as deleted
        controller->arrays[array_id].initialized = 0;
    }
    
    release_raid_lock();
    
    return 0;
}

uint32_t raid_read_sectors(uint32_t controller_id, uint32_t array_id,
                          uint64_t lba, uint32_t count, void *buffer) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return 1;
    }
    
    raid_controller_t *controller = &raid_controllers[controller_id];
    raid_array_t *array = &controller->arrays[array_id];
    
    if (!array->initialized) {
        return 2;
    }
    
    uint32_t result = 0;
    
    acquire_raid_lock();
    
    switch (array->level) {
        case RAID_LEVEL_0:
            result = raid0_read(array, lba, count, buffer);
            break;
        
        case RAID_LEVEL_1:
            result = raid1_read(array, lba, count, buffer);
            break;
        
        case RAID_LEVEL_5:
            result = raid5_read(array, lba, count, buffer);
            break;
        
        default:
            result = 3;  // Unsupported level
            break;
    }
    
    if (result == 0) {
        array->reads++;
        controller->total_read_ops++;
    } else {
        array->errors++;
        controller->total_errors++;
    }
    
    release_raid_lock();
    
    return result;
}

uint32_t raid_write_sectors(uint32_t controller_id, uint32_t array_id,
                           uint64_t lba, uint32_t count, void *buffer) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return 1;
    }
    
    raid_controller_t *controller = &raid_controllers[controller_id];
    raid_array_t *array = &controller->arrays[array_id];
    
    if (!array->initialized) {
        return 2;
    }
    
    uint32_t result = 0;
    
    acquire_raid_lock();
    
    switch (array->level) {
        case RAID_LEVEL_0:
            result = raid0_write(array, lba, count, buffer);
            break;
        
        case RAID_LEVEL_1:
            result = raid1_write(array, lba, count, buffer);
            break;
        
        case RAID_LEVEL_5:
            result = raid5_write(array, lba, count, buffer);
            break;
        
        default:
            result = 3;
            break;
    }
    
    if (result == 0) {
        array->writes++;
        controller->total_write_ops++;
    } else {
        array->errors++;
        controller->total_errors++;
    }
    
    release_raid_lock();
    
    return result;
}

uint32_t raid_flush_cache(uint32_t controller_id, uint32_t array_id) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return 1;
    }
    
    raid_controller_t *controller = &raid_controllers[controller_id];
    raid_array_t *array = &controller->arrays[array_id];
    
    // Flush all member drives
    for (uint32_t i = 0; i < array->drive_count; i++) {
        disk_flush(array->drives[i].disk_id);
    }
    
    return 0;
}

uint32_t raid_get_array_info(uint32_t controller_id, uint32_t array_id, raid_array_t *info) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS || !info) {
        return 1;
    }
    
    raid_controller_t *controller = &raid_controllers[controller_id];
    memcpy(info, &controller->arrays[array_id], sizeof(raid_array_t));
    
    return 0;
}

uint32_t raid_get_array_status(uint32_t controller_id, uint32_t array_id) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return RAID_STATE_FAILED;
    }
    
    return raid_controllers[controller_id].arrays[array_id].state;
}

uint32_t raid_get_rebuild_progress(uint32_t controller_id, uint32_t array_id, uint32_t *percent) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS || !percent) {
        return 1;
    }
    
    raid_array_t *array = &raid_controllers[controller_id].arrays[array_id];
    
    if (array->total_sectors == 0) {
        *percent = 0;
        return 2;
    }
    
    *percent = (array->rebuild_progress * 100) / array->total_sectors;
    
    return 0;
}

uint32_t raid_get_stats(uint32_t controller_id, uint32_t array_id,
                       uint64_t *reads, uint64_t *writes, uint64_t *errors) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return 1;
    }
    
    raid_array_t *array = &raid_controllers[controller_id].arrays[array_id];
    
    *reads = array->reads;
    *writes = array->writes;
    *errors = array->errors;
    
    return 0;
}

// Stub implementations for remaining operations
uint32_t raid_start_rebuild(uint32_t controller_id, uint32_t array_id) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return 1;
    }
    
    raid_controllers[controller_id].arrays[array_id].state = RAID_STATE_REBUILDING;
    return 0;
}

uint32_t raid_cancel_rebuild(uint32_t controller_id, uint32_t array_id) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return 1;
    }
    
    raid_controllers[controller_id].arrays[array_id].state = RAID_STATE_NORMAL;
    return 0;
}

uint32_t raid_add_spare_drive(uint32_t controller_id, uint32_t array_id, uint32_t disk_id) {
    // TODO: Implement spare drive management
    return 0;
}

uint32_t raid_remove_drive(uint32_t controller_id, uint32_t array_id, uint32_t drive_index) {
    // TODO: Implement drive removal
    return 0;
}

uint32_t raid_set_drive_online(uint32_t controller_id, uint32_t array_id, uint32_t drive_index) {
    if (controller_id >= raid_controller_count || array_id >= MAX_RAID_ARRAYS) {
        return 1;
    }
    
    raid_array_t *array = &raid_controllers[controller_id].arrays[array_id];
    
    if (drive_index >= array->drive_count) {
        return 2;
    }
    
    array->drives[drive_index].state = RAID_DRIVE_ONLINE;
    
    return 0;
}
