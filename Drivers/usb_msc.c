#include "usb_msc.h"
#include <string.h>

// ============================
// USB MASS STORAGE DEVICE POOL
// ============================

#define MAX_USB_MASS_STORAGE_DEVICES 16
usb_mass_storage_device_t usb_msc_devices[MAX_USB_MASS_STORAGE_DEVICES];
uint32_t usb_msc_device_count = 0;
uint32_t usb_msc_lock = 0;

// ============================
// UTILITY FUNCTIONS
// ============================

static inline void acquire_usb_msc_lock(void) {
    while (__sync_lock_test_and_set(&usb_msc_lock, 1)) {
        __asm__ volatile ("pause");
    }
}

static inline void release_usb_msc_lock(void) {
    __sync_lock_release(&usb_msc_lock);
}

static void usb_msc_delay_ms(uint32_t ms) {
    volatile uint32_t counter = ms * 1000;
    while (counter--) {
        __asm__ volatile ("pause");
    }
}

// ============================
// USB BULK TRANSFER
// ============================

static uint32_t usb_bulk_transfer_out(usb_mass_storage_device_t *dev, void *data, uint32_t length) {
    // TODO: Implement USB bulk transfer out
    // This would require USB host controller driver integration
    return 0;
}

static uint32_t usb_bulk_transfer_in(usb_mass_storage_device_t *dev, void *data, uint32_t length) {
    // TODO: Implement USB bulk transfer in
    return 0;
}

// ============================
// USB MASS STORAGE TRANSFERS
// ============================

static uint32_t usb_msc_send_cbw(usb_mass_storage_device_t *dev, usb_cbw_t *cbw) {
    cbw->signature = USB_CBW_SIGNATURE;
    cbw->tag = dev->command_tag++;
    
    // Send CBW
    if (usb_bulk_transfer_out(dev, cbw, sizeof(usb_cbw_t)) != 0) {
        dev->total_errors++;
        return 1;
    }
    
    return 0;
}

static uint32_t usb_msc_receive_csw(usb_mass_storage_device_t *dev, usb_csw_t *csw) {
    // Receive CSW
    if (usb_bulk_transfer_in(dev, csw, sizeof(usb_csw_t)) != 0) {
        dev->total_errors++;
        return 1;
    }
    
    if (csw->signature != USB_CSW_SIGNATURE) {
        dev->total_errors++;
        return 2;  // Invalid CSW signature
    }
    
    if (csw->status != 0) {
        dev->total_errors++;
        return csw->status + 10;  // Command failed
    }
    
    return 0;
}

// ============================
// SCSI COMMAND IMPLEMENTATION
// ============================

uint32_t usb_msc_test_unit_ready(usb_mass_storage_device_t *dev) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    cbw.lun = 0;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_CMD_TEST_UNIT_READY;
    cbw.data_transfer_len = 0;
    cbw.flags = 0x80;  // Device to host
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    // No data phase for test unit ready
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 2;
    }
    
    dev->ready = 1;
    return 0;
}

uint32_t usb_msc_inquiry(usb_mass_storage_device_t *dev, void *buffer) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    uint32_t allocation_length = 36;
    
    cbw.lun = 0;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_CMD_INQUIRY;
    cbw.cb[4] = allocation_length;
    cbw.data_transfer_len = allocation_length;
    cbw.flags = 0x80;  // Device to host
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    // Data phase
    if (usb_bulk_transfer_in(dev, buffer, allocation_length) != 0) {
        dev->total_errors++;
        return 2;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 3;
    }
    
    return 0;
}

uint32_t usb_msc_read_capacity(usb_mass_storage_device_t *dev) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    scsi_read_capacity_response_t capacity = {0};
    
    cbw.lun = 0;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_CMD_READ_CAPACITY_10;
    cbw.data_transfer_len = sizeof(scsi_read_capacity_response_t);
    cbw.flags = 0x80;  // Device to host
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    // Data phase
    if (usb_bulk_transfer_in(dev, &capacity, sizeof(scsi_read_capacity_response_t)) != 0) {
        dev->total_errors++;
        return 2;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 3;
    }
    
    // Parse capacity
    dev->total_sectors = ((uint32_t)capacity.last_block_address + 1);
    dev->sector_size = capacity.block_size;
    
    return 0;
}

uint32_t usb_msc_request_sense(usb_mass_storage_device_t *dev, void *buffer) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    uint32_t allocation_length = 18;
    
    cbw.lun = 0;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_CMD_REQUEST_SENSE;
    cbw.cb[4] = allocation_length;
    cbw.data_transfer_len = allocation_length;
    cbw.flags = 0x80;  // Device to host
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    // Data phase
    if (usb_bulk_transfer_in(dev, buffer, allocation_length) != 0) {
        dev->total_errors++;
        return 2;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 3;
    }
    
    return 0;
}

uint32_t usb_msc_read_10(usb_mass_storage_device_t *dev, uint32_t lba, uint16_t block_count, void *buffer) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    uint32_t transfer_length = block_count * dev->sector_size;
    
    cbw.lun = 0;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_CMD_READ_10;
    cbw.cb[2] = (lba >> 24) & 0xFF;
    cbw.cb[3] = (lba >> 16) & 0xFF;
    cbw.cb[4] = (lba >> 8) & 0xFF;
    cbw.cb[5] = lba & 0xFF;
    cbw.cb[7] = (block_count >> 8) & 0xFF;
    cbw.cb[8] = block_count & 0xFF;
    cbw.data_transfer_len = transfer_length;
    cbw.flags = 0x80;  // Device to host
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    // Data phase
    if (usb_bulk_transfer_in(dev, buffer, transfer_length) != 0) {
        dev->total_errors++;
        return 2;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 3;
    }
    
    dev->total_reads += block_count;
    return 0;
}

uint32_t usb_msc_read_16(usb_mass_storage_device_t *dev, uint64_t lba, uint32_t block_count, void *buffer) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    uint32_t transfer_length = block_count * dev->sector_size;
    
    cbw.lun = 0;
    cbw.cb_length = 16;
    cbw.cb[0] = SCSI_CMD_READ_16;
    cbw.cb[2] = (lba >> 56) & 0xFF;
    cbw.cb[3] = (lba >> 48) & 0xFF;
    cbw.cb[4] = (lba >> 40) & 0xFF;
    cbw.cb[5] = (lba >> 32) & 0xFF;
    cbw.cb[6] = (lba >> 24) & 0xFF;
    cbw.cb[7] = (lba >> 16) & 0xFF;
    cbw.cb[8] = (lba >> 8) & 0xFF;
    cbw.cb[9] = lba & 0xFF;
    cbw.cb[10] = (block_count >> 24) & 0xFF;
    cbw.cb[11] = (block_count >> 16) & 0xFF;
    cbw.cb[12] = (block_count >> 8) & 0xFF;
    cbw.cb[13] = block_count & 0xFF;
    cbw.data_transfer_len = transfer_length;
    cbw.flags = 0x80;  // Device to host
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    // Data phase
    if (usb_bulk_transfer_in(dev, buffer, transfer_length) != 0) {
        dev->total_errors++;
        return 2;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 3;
    }
    
    dev->total_reads += block_count;
    return 0;
}

uint32_t usb_msc_write_10(usb_mass_storage_device_t *dev, uint32_t lba, uint16_t block_count, void *buffer) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    uint32_t transfer_length = block_count * dev->sector_size;
    
    cbw.lun = 0;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_CMD_WRITE_10;
    cbw.cb[2] = (lba >> 24) & 0xFF;
    cbw.cb[3] = (lba >> 16) & 0xFF;
    cbw.cb[4] = (lba >> 8) & 0xFF;
    cbw.cb[5] = lba & 0xFF;
    cbw.cb[7] = (block_count >> 8) & 0xFF;
    cbw.cb[8] = block_count & 0xFF;
    cbw.data_transfer_len = transfer_length;
    cbw.flags = 0x00;  // Host to device
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    // Data phase
    if (usb_bulk_transfer_out(dev, buffer, transfer_length) != 0) {
        dev->total_errors++;
        return 2;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 3;
    }
    
    dev->total_writes += block_count;
    return 0;
}

uint32_t usb_msc_write_16(usb_mass_storage_device_t *dev, uint64_t lba, uint32_t block_count, void *buffer) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    uint32_t transfer_length = block_count * dev->sector_size;
    
    cbw.lun = 0;
    cbw.cb_length = 16;
    cbw.cb[0] = SCSI_CMD_WRITE_16;
    cbw.cb[2] = (lba >> 56) & 0xFF;
    cbw.cb[3] = (lba >> 48) & 0xFF;
    cbw.cb[4] = (lba >> 40) & 0xFF;
    cbw.cb[5] = (lba >> 32) & 0xFF;
    cbw.cb[6] = (lba >> 24) & 0xFF;
    cbw.cb[7] = (lba >> 16) & 0xFF;
    cbw.cb[8] = (lba >> 8) & 0xFF;
    cbw.cb[9] = lba & 0xFF;
    cbw.cb[10] = (block_count >> 24) & 0xFF;
    cbw.cb[11] = (block_count >> 16) & 0xFF;
    cbw.cb[12] = (block_count >> 8) & 0xFF;
    cbw.cb[13] = block_count & 0xFF;
    cbw.data_transfer_len = transfer_length;
    cbw.flags = 0x00;  // Host to device
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    // Data phase
    if (usb_bulk_transfer_out(dev, buffer, transfer_length) != 0) {
        dev->total_errors++;
        return 2;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 3;
    }
    
    dev->total_writes += block_count;
    return 0;
}

uint32_t usb_msc_start_stop_unit(usb_mass_storage_device_t *dev, uint8_t start) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    cbw.lun = 0;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_CMD_START_STOP_UNIT;
    cbw.cb[4] = start ? 0x01 : 0x02;
    cbw.data_transfer_len = 0;
    cbw.flags = 0x80;
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 2;
    }
    
    return 0;
}

uint32_t usb_msc_synchronize_cache(usb_mass_storage_device_t *dev) {
    usb_cbw_t cbw = {0};
    usb_csw_t csw = {0};
    
    cbw.lun = 0;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_CMD_SYNCHRONIZE_CACHE;
    cbw.data_transfer_len = 0;
    cbw.flags = 0x80;
    
    if (usb_msc_send_cbw(dev, &cbw) != 0) {
        return 1;
    }
    
    if (usb_msc_receive_csw(dev, &csw) != 0) {
        return 2;
    }
    
    return 0;
}

uint32_t usb_msc_reset(usb_mass_storage_device_t *dev) {
    // TODO: Implement USB mass storage reset
    return 0;
}

uint32_t usb_msc_get_stats(usb_mass_storage_device_t *dev, uint64_t *reads, uint64_t *writes, uint64_t *errors) {
    *reads = dev->total_reads;
    *writes = dev->total_writes;
    *errors = dev->total_errors;
    
    return 0;
}

// ============================
// PUBLIC INTERFACE
// ============================

uint32_t usb_msc_init(void) {
    memset(usb_msc_devices, 0, sizeof(usb_msc_devices));
    usb_msc_device_count = 0;
    
    // TODO: Enumerate USB devices and find mass storage devices
    
    return 0;
}

uint32_t usb_msc_probe_device(uint8_t bus, uint8_t address, uint8_t interface) {
    if (usb_msc_device_count >= MAX_USB_MASS_STORAGE_DEVICES) {
        return 1;
    }
    
    usb_mass_storage_device_t *dev = &usb_msc_devices[usb_msc_device_count];
    
    dev->device_id = usb_msc_device_count;
    dev->usb_bus = bus;
    dev->usb_address = address;
    dev->usb_interface = interface;
    
    // TODO: Read endpoint descriptors from USB configuration
    dev->bulk_in_endpoint = 0x81;
    dev->bulk_out_endpoint = 0x01;
    dev->interrupt_endpoint = 0;
    
    dev->max_packet_size_in = 512;
    dev->max_packet_size_out = 512;
    
    dev->command_tag = 1;
    dev->state = 0;
    
    // Test unit ready
    if (usb_msc_test_unit_ready(dev) != 0) {
        return 2;
    }
    
    // Read capacity
    if (usb_msc_read_capacity(dev) != 0) {
        return 3;
    }
    
    usb_msc_device_count++;
    return 0;
}
