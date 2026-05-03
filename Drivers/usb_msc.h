#pragma once

#include <stdint.h>

// ============================
// USB MASS STORAGE DEFINITIONS
// ============================

// USB Class Code
#define USB_CLASS_MASS_STORAGE 0x08

// USB Subclass Code
#define USB_SUBCLASS_SCSI 0x06
#define USB_SUBCLASS_RBC 0x01
#define USB_SUBCLASS_UFI 0x04

// USB Protocol Code
#define USB_PROTOCOL_BBB 0x50  // Bulk-Only Transport
#define USB_PROTOCOL_CBI 0x00  // Control/Bulk/Interrupt

// CBW (Command Block Wrapper) signature
#define USB_CBW_SIGNATURE 0x43425355

// CSW (Command Status Wrapper) signature
#define USB_CSW_SIGNATURE 0x53425355

// SCSI Commands
#define SCSI_CMD_TEST_UNIT_READY 0x00
#define SCSI_CMD_REQUEST_SENSE 0x03
#define SCSI_CMD_INQUIRY 0x12
#define SCSI_CMD_READ_CAPACITY_10 0x25
#define SCSI_CMD_READ_CAPACITY_16 0x9E
#define SCSI_CMD_READ_10 0x28
#define SCSI_CMD_READ_16 0x88
#define SCSI_CMD_WRITE_10 0x2A
#define SCSI_CMD_WRITE_16 0x8A
#define SCSI_CMD_START_STOP_UNIT 0x1B
#define SCSI_CMD_SYNCHRONIZE_CACHE 0x35

// ============================
// USB CBW STRUCTURE
// ============================

typedef struct {
    uint32_t signature;         // 0x43425355
    uint32_t tag;
    uint32_t data_transfer_len;
    uint8_t flags;              // Bit 7: direction (0=out, 1=in)
    uint8_t lun;
    uint8_t cb_length;          // Command block length (1-16)
    uint8_t cb[16];             // Command block
} __attribute__((packed)) usb_cbw_t;

// ============================
// USB CSW STRUCTURE
// ============================

typedef struct {
    uint32_t signature;         // 0x53425355
    uint32_t tag;
    uint32_t data_residue;
    uint8_t status;             // 0=success, 1=command failed, 2=phase error
} __attribute__((packed)) usb_csw_t;

// ============================
// SCSI STRUCTURES
// ============================

typedef struct {
    uint8_t device_type;
    uint8_t removable;
    uint8_t version;
    uint8_t response_data_format;
    uint8_t additional_length;
    uint8_t reserved[3];
    uint8_t vendor_id[8];
    uint8_t product_id[16];
    uint8_t product_revision[4];
} __attribute__((packed)) scsi_inquiry_response_t;

typedef struct {
    uint32_t last_block_address;
    uint32_t block_size;
} __attribute__((packed)) scsi_read_capacity_response_t;

typedef struct {
    uint8_t error_code;
    uint8_t segment_number;
    uint8_t sense_key;
    uint32_t information;
    uint8_t additional_sense_length;
    uint32_t command_specific_info;
    uint8_t additional_sense_code;
    uint8_t additional_sense_qualifier;
    uint8_t fru_code;
    uint8_t sense_key_specific[3];
} __attribute__((packed)) scsi_sense_response_t;

// ============================
// USB MASS STORAGE DEVICE
// ============================

typedef struct {
    uint32_t device_id;
    uint8_t usb_bus;
    uint8_t usb_address;
    uint8_t usb_interface;
    
    // Endpoint addresses
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
    uint8_t interrupt_endpoint;
    
    // Transfer sizes
    uint16_t max_packet_size_in;
    uint16_t max_packet_size_out;
    
    // Device info
    uint8_t subclass;
    uint8_t protocol;
    
    // Capacity
    uint64_t total_sectors;
    uint32_t sector_size;
    
    // Command tracking
    uint32_t command_tag;
    uint32_t last_tag;
    
    // Statistics
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_errors;
    
    // Device state
    uint32_t state;
    uint32_t ready;
} usb_mass_storage_device_t;

// ============================
// FUNCTION PROTOTYPES
// ============================

// Initialization
uint32_t usb_msc_init(void);
uint32_t usb_msc_probe_device(uint8_t bus, uint8_t address, uint8_t interface);

// SCSI commands
uint32_t usb_msc_test_unit_ready(usb_mass_storage_device_t *dev);
uint32_t usb_msc_inquiry(usb_mass_storage_device_t *dev, void *buffer);
uint32_t usb_msc_read_capacity(usb_mass_storage_device_t *dev);
uint32_t usb_msc_request_sense(usb_mass_storage_device_t *dev, void *buffer);

// Data operations
uint32_t usb_msc_read_10(usb_mass_storage_device_t *dev, uint32_t lba, uint16_t block_count, void *buffer);
uint32_t usb_msc_read_16(usb_mass_storage_device_t *dev, uint64_t lba, uint32_t block_count, void *buffer);
uint32_t usb_msc_write_10(usb_mass_storage_device_t *dev, uint32_t lba, uint16_t block_count, void *buffer);
uint32_t usb_msc_write_16(usb_mass_storage_device_t *dev, uint64_t lba, uint32_t block_count, void *buffer);

// Device control
uint32_t usb_msc_start_stop_unit(usb_mass_storage_device_t *dev, uint8_t start);
uint32_t usb_msc_synchronize_cache(usb_mass_storage_device_t *dev);
uint32_t usb_msc_reset(usb_mass_storage_device_t *dev);

// Statistics
uint32_t usb_msc_get_stats(usb_mass_storage_device_t *dev, uint64_t *reads, uint64_t *writes, uint64_t *errors);

#endif // USB_MASS_STORAGE_HEADER
