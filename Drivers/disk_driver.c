#include "disk_driver.h"
#include <stdint.h>
#include <string.h>

// ============================
// GLOBAL VARIABLES
// ============================

disk_device_t disk_devices[MAX_DISK_DEVICES];
uint32_t disk_count = 0;
volatile uint32_t disk_irq_status = 0;
uint32_t disk_lock = 0;

static uint8_t disk_cache_data[MAX_DISK_DEVICES][DISK_CACHE_BLOCKS][DISK_SECTOR_SIZE];

// ============================
// UTILITY FUNCTIONS
// ============================

static inline void acquire_disk_lock(void) {
    while (__sync_lock_test_and_set(&disk_lock, 1)) {
        __asm__ volatile ("pause");
    }
}

static inline void release_disk_lock(void) {
    __sync_lock_release(&disk_lock);
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint32_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint32_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static void busy_wait(uint32_t ms) {
    volatile uint32_t counter = ms * 1000;
    while (counter--) {
        __asm__ volatile ("pause");
    }
}

static uint32_t disk_get_current_time_ms(void) {
    // TODO: implement real timer
    return 0;
}

static chs_t lba_to_chs(uint64_t lba, disk_device_t *disk) {
    chs_t chs = {0};
    
    if (lba >= (uint64_t)disk->cylinders * disk->heads * disk->sectors_per_track) {
        // LBA out of bounds, use maximum CHS
        chs.cylinder = disk->cylinders - 1;
        chs.head = disk->heads - 1;
        chs.sector = disk->sectors_per_track;
        return chs;
    }
    
    chs.sector = (lba % disk->sectors_per_track) + 1;
    uint64_t temp = lba / disk->sectors_per_track;
    chs.head = temp % disk->heads;
    chs.cylinder = (temp / disk->heads) & 0xFFFF;
    
    return chs;
}

// ============================
// IDE DRIVER FUNCTIONS
// ============================

static uint32_t ide_wait_status(uint16_t base, uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    
    while (elapsed < timeout_ms) {
        uint8_t status = inb(base + IDE_STATUS_CMD);
        
        if (!(status & ATA_STATUS_BSY)) {
            if (status & ATA_STATUS_ERR) {
                return 1;  // Error
            }
            return 0;  // Success
        }
        
        busy_wait(1);
        elapsed++;
    }
    
    return 2;  // Timeout
}

static uint32_t ide_read_sectors_pio(disk_device_t *disk, uint64_t lba, uint32_t count, void *buffer) {
    uint16_t base = disk->cmd_base;
    uint8_t *buf = (uint8_t *)buffer;
    uint32_t sectors_read = 0;
    
    if (!disk->lba48_capable && lba > 0xFFFFFFF) {
        return 3;  // LBA48 required
    }
    
    // Select device
    uint8_t device_byte = 0xE0 | ((disk->device_num & 1) << 4);
    if (!disk->lba48_capable) {
        // CHS mode
        chs_t chs = lba_to_chs(lba, disk);
        device_byte |= (chs.head & 0x0F);
        outb(base + IDE_HEAD, device_byte);
        outb(base + IDE_SECTOR_NUM, chs.sector);
        outb(base + IDE_CYLINDER_LOW, chs.cylinder & 0xFF);
        outb(base + IDE_CYLINDER_HIGH, (chs.cylinder >> 8) & 0xFF);
    } else {
        // LBA48 mode
        device_byte |= 0x40;
        outb(base + IDE_HEAD, device_byte);
        outb(base + IDE_SECTOR_COUNT, (count >> 8) & 0xFF);
        outb(base + IDE_SECTOR_NUM, (lba >> 24) & 0xFF);
        outb(base + IDE_CYLINDER_LOW, (lba >> 32) & 0xFF);
        outb(base + IDE_CYLINDER_HIGH, (lba >> 40) & 0xFF);
        outb(base + IDE_SECTOR_COUNT, count & 0xFF);
        outb(base + IDE_SECTOR_NUM, lba & 0xFF);
        outb(base + IDE_CYLINDER_LOW, (lba >> 8) & 0xFF);
        outb(base + IDE_CYLINDER_HIGH, (lba >> 16) & 0xFF);
    }
    
    // Issue read command
    uint8_t cmd = (disk->lba48_capable && count <= 256) ? ATA_CMD_READ_DMA_EXT : ATA_CMD_READ_DMA;
    if (count > 256) {
        cmd = ATA_CMD_READ_DMA_EXT;
    }
    outb(base + IDE_STATUS_CMD, cmd);
    
    // Wait for operation
    if (ide_wait_status(base, DISK_TIMEOUT_MS) != 0) {
        disk->total_errors++;
        disk->last_error_code = inb(base + IDE_ERROR);
        return inb(base + IDE_ERROR);
    }
    
    // Read data
    for (uint32_t i = 0; i < count; i++) {
        for (uint16_t j = 0; j < DISK_SECTOR_SIZE / 2; j++) {
            uint16_t data = inw(base + IDE_DATA);
            buf[i * DISK_SECTOR_SIZE + j * 2] = data & 0xFF;
            buf[i * DISK_SECTOR_SIZE + j * 2 + 1] = (data >> 8) & 0xFF;
        }
        sectors_read++;
    }
    
    disk->total_reads += sectors_read;
    return 0;
}

static uint32_t ide_write_sectors_pio(disk_device_t *disk, uint64_t lba, uint32_t count, void *buffer) {
    uint16_t base = disk->cmd_base;
    uint8_t *buf = (uint8_t *)buffer;
    uint32_t sectors_written = 0;
    
    if (!disk->lba48_capable && lba > 0xFFFFFFF) {
        return 3;
    }
    
    // Select device
    uint8_t device_byte = 0xE0 | ((disk->device_num & 1) << 4);
    if (!disk->lba48_capable) {
        chs_t chs = lba_to_chs(lba, disk);
        device_byte |= (chs.head & 0x0F);
        outb(base + IDE_HEAD, device_byte);
        outb(base + IDE_SECTOR_NUM, chs.sector);
        outb(base + IDE_CYLINDER_LOW, chs.cylinder & 0xFF);
        outb(base + IDE_CYLINDER_HIGH, (chs.cylinder >> 8) & 0xFF);
    } else {
        device_byte |= 0x40;
        outb(base + IDE_HEAD, device_byte);
        outb(base + IDE_SECTOR_COUNT, (count >> 8) & 0xFF);
        outb(base + IDE_SECTOR_NUM, (lba >> 24) & 0xFF);
        outb(base + IDE_CYLINDER_LOW, (lba >> 32) & 0xFF);
        outb(base + IDE_CYLINDER_HIGH, (lba >> 40) & 0xFF);
        outb(base + IDE_SECTOR_COUNT, count & 0xFF);
        outb(base + IDE_SECTOR_NUM, lba & 0xFF);
        outb(base + IDE_CYLINDER_LOW, (lba >> 8) & 0xFF);
        outb(base + IDE_CYLINDER_HIGH, (lba >> 16) & 0xFF);
    }
    
    // Issue write command
    outb(base + IDE_STATUS_CMD, ATA_CMD_WRITE_DMA);
    
    // Write data
    for (uint32_t i = 0; i < count; i++) {
        for (uint16_t j = 0; j < DISK_SECTOR_SIZE / 2; j++) {
            uint16_t data = buf[i * DISK_SECTOR_SIZE + j * 2] |
                           (buf[i * DISK_SECTOR_SIZE + j * 2 + 1] << 8);
            outw(base + IDE_DATA, data);
        }
        sectors_written++;
    }
    
    // Wait for completion
    if (ide_wait_status(base, DISK_TIMEOUT_MS) != 0) {
        disk->total_errors++;
        disk->last_error_code = inb(base + IDE_ERROR);
        return inb(base + IDE_ERROR);
    }
    
    // Flush cache
    outb(base + IDE_STATUS_CMD, ATA_CMD_FLUSH_CACHE_EXT);
    ide_wait_status(base, DISK_TIMEOUT_MS);
    
    disk->total_writes += sectors_written;
    return 0;
}

static uint32_t ide_identify_device(disk_device_t *disk) {
    uint16_t base = disk->cmd_base;
    uint16_t *identify_data = (uint16_t *)disk->dma_buffer;
    
    // Select device
    outb(base + IDE_HEAD, 0xE0 | ((disk->device_num & 1) << 4));
    busy_wait(1);
    
    // Issue IDENTIFY command
    outb(base + IDE_STATUS_CMD, ATA_CMD_IDENTIFY);
    
    if (ide_wait_status(base, DISK_TIMEOUT_MS) != 0) {
        return 1;
    }
    
    // Read identification data
    for (uint16_t i = 0; i < DISK_SECTOR_SIZE / 2; i++) {
        identify_data[i] = inw(base + IDE_DATA);
    }
    
    // Parse identification data
    disk->cylinders = identify_data[1];
    disk->heads = identify_data[3];
    disk->sectors_per_track = identify_data[6];
    
    // Check for 48-bit LBA support
    if (identify_data[83] & 0x0400) {
        disk->lba48_capable = 1;
        disk->total_sectors = *(uint64_t *)&identify_data[100];
    } else {
        disk->lba48_capable = 0;
        disk->total_sectors = *(uint32_t *)&identify_data[60];
    }
    
    // Check DMA support
    disk->dma_capable = (identify_data[49] & 0x0100) ? 1 : 0;
    
    // Check SMART support
    disk->smart_capable = (identify_data[82] & 0x0001) ? 1 : 0;
    
    disk->bytes_per_sector = DISK_SECTOR_SIZE;
    disk->state = DISK_STATE_READY;
    
    return 0;
}

// ============================
// AHCI DRIVER FUNCTIONS
// ============================

static uint32_t ahci_port_reset(disk_device_t *disk) {
    volatile uint32_t *port_regs = (volatile uint32_t *)(disk->ahci_base + 0x100 + (disk->port_num * 0x80));
    
    // Clear command and status
    port_regs[AHCI_PORT_CMD / 4] = 0;
    busy_wait(100);
    
    // Restart port
    port_regs[AHCI_PORT_SCTL / 4] = 0x00000001;
    busy_wait(100);
    port_regs[AHCI_PORT_SCTL / 4] = 0x00000000;
    busy_wait(100);
    
    // Wait for port to be ready
    uint32_t elapsed = 0;
    while ((port_regs[AHCI_PORT_SSTS / 4] & 0x0F) != 0x03 && elapsed < DISK_TIMEOUT_MS) {
        busy_wait(1);
        elapsed++;
    }
    
    if ((port_regs[AHCI_PORT_SSTS / 4] & 0x0F) != 0x03) {
        return 1;  // Port not ready
    }
    
    // Clear errors
    port_regs[AHCI_PORT_SERR / 4] = 0xFFFFFFFF;
    port_regs[AHCI_PORT_IS / 4] = 0xFFFFFFFF;
    
    return 0;
}

static uint32_t ahci_send_cmd(disk_device_t *disk, uint8_t cmd, uint64_t lba, uint32_t count, void *buffer) {
    volatile uint32_t *port_regs = (volatile uint32_t *)(disk->ahci_base + 0x100 + (disk->port_num * 0x80));
    
    // Find free command slot
    uint32_t ci = port_regs[AHCI_PORT_CI / 4];
    int cmd_slot = -1;
    for (int i = 0; i < 32; i++) {
        if (!(ci & (1 << i))) {
            cmd_slot = i;
            break;
        }
    }
    
    if (cmd_slot < 0) {
        return 1;  // No free slot
    }
    
    // Build command FIS
    ahci_cmd_fis_t *fis = (ahci_cmd_fis_t *)disk->dma_buffer;
    memset(fis, 0, sizeof(ahci_cmd_fis_t));
    
    fis->fis_type = 0x27;  // Register H2D FIS
    fis->flags = 0x80;     // Command bit
    fis->cmd = cmd;
    fis->lba = lba;
    fis->count = count;
    
    if (cmd == ATA_CMD_READ_DMA_EXT || cmd == ATA_CMD_WRITE_DMA_EXT) {
        fis->features_low = 0;
    }
    
    // Update command header
    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t *)(disk->dma_buffer + 0x1000 + (cmd_slot * 0x20));
    cmd_header->cmd_table_addr_low = (uint32_t)(disk->dma_physical_addr + 0x800);
    cmd_header->cmd_table_addr_high = (uint32_t)((disk->dma_physical_addr + 0x800) >> 32);
    
    // Issue command
    port_regs[AHCI_PORT_CI / 4] = (1 << cmd_slot);
    
    // Wait for completion
    uint32_t elapsed = 0;
    while ((port_regs[AHCI_PORT_CI / 4] & (1 << cmd_slot)) && elapsed < DISK_TIMEOUT_MS) {
        busy_wait(1);
        elapsed++;
    }
    
    if (elapsed >= DISK_TIMEOUT_MS) {
        return 2;  // Timeout
    }
    
    // Check for errors
    if (port_regs[AHCI_PORT_IS / 4] & 0xFD8E0001) {
        port_regs[AHCI_PORT_SERR / 4] = 0xFFFFFFFF;
        port_regs[AHCI_PORT_IS / 4] = 0xFFFFFFFF;
        disk->total_errors++;
        return port_regs[AHCI_PORT_TFD / 4] & 0xFF;
    }
    
    return 0;
}

static uint32_t ahci_identify_device(disk_device_t *disk) {
    if (ahci_send_cmd(disk, ATA_CMD_IDENTIFY, 0, 1, disk->dma_buffer) != 0) {
        return 1;
    }
    
    uint16_t *identify_data = (uint16_t *)disk->dma_buffer;
    
    // Parse identification data
    disk->cylinders = identify_data[1];
    disk->heads = identify_data[3];
    disk->sectors_per_track = identify_data[6];
    disk->dma_capable = 1;  // AHCI implies DMA
    disk->lba48_capable = (identify_data[83] & 0x0400) ? 1 : 0;
    disk->smart_capable = (identify_data[82] & 0x0001) ? 1 : 0;
    disk->ncq_capable = (identify_data[76] & 0x0100) ? 1 : 0;
    
    if (disk->lba48_capable) {
        disk->total_sectors = *(uint64_t *)&identify_data[100];
    } else {
        disk->total_sectors = *(uint32_t *)&identify_data[60];
    }
    
    disk->bytes_per_sector = DISK_SECTOR_SIZE;
    disk->state = DISK_STATE_READY;
    
    return 0;
}

// ============================
// CACHE MANAGEMENT
// ============================

static uint32_t disk_cache_lookup(disk_device_t *disk, uint64_t lba) {
    for (uint32_t i = 0; i < disk->cache_size; i++) {
        if ((disk->cache[i].flags & CACHE_VALID) && disk->cache[i].lba == lba) {
            disk->cache_hits++;
            return i;
        }
    }
    
    disk->cache_misses++;
    return 0xFFFFFFFF;
}

static uint32_t disk_cache_allocate(disk_device_t *disk, uint64_t lba) {
    // Find empty slot
    for (uint32_t i = 0; i < disk->cache_size; i++) {
        if (!(disk->cache[i].flags & CACHE_VALID)) {
            disk->cache[i].lba = lba;
            disk->cache[i].flags = CACHE_VALID;
            disk->cache[i].size = DISK_SECTOR_SIZE;
            return i;
        }
    }
    
    // Evict least recently used if cache is full
    uint32_t lru_idx = 0;
    uint32_t min_access = 0xFFFFFFFF;
    
    for (uint32_t i = 0; i < disk->cache_size; i++) {
        if (!(disk->cache[i].flags & CACHE_LOCKED) && disk->cache[i].flags < min_access) {
            min_access = disk->cache[i].flags;
            lru_idx = i;
        }
    }
    
    // If dirty, flush to disk
    if (disk->cache[lru_idx].flags & CACHE_DIRTY) {
        disk_write_sectors(disk->device_id, disk->cache[lru_idx].lba, 1,
            (void *)((uint64_t)disk->dma_buffer + lru_idx * DISK_SECTOR_SIZE));
    }
    
    disk->cache[lru_idx].lba = lba;
    disk->cache[lru_idx].flags = CACHE_VALID;
    return lru_idx;
}

// ============================
// PUBLIC INTERFACE
// ============================

uint32_t disk_init(void) {
    memset(disk_devices, 0, sizeof(disk_devices));
    disk_count = 0;
    
    // TODO: Initialize DMA controller
    // TODO: Register IRQ handlers
    
    return disk_detect_devices();
}

uint32_t disk_detect_devices(void) {
    // TODO: Scan PCI bus for disk controllers
    // For now, return success
    return 0;
}

uint32_t disk_probe_device(uint16_t bus, uint16_t device, uint16_t function) {
    if (disk_count >= MAX_DISK_DEVICES) {
        return 1;
    }
    
    disk_device_t *disk = &disk_devices[disk_count];
    disk->device_id = disk_count;
    disk->bus_num = bus;
    disk->device_num = device;
    disk->function_num = function;
    
    // TODO: Read PCI configuration to determine controller type
    // For now, assume IDE
    disk->type = DISK_TYPE_IDE;
    disk->cmd_base = IDE_PRIMARY_CMD;
    disk->ctl_base = IDE_PRIMARY_CTL;
    
    // Allocate DMA buffer
    disk->dma_buffer = (uint8_t *)(0x1000000 + (disk_count * DMA_BUFFER_SIZE));
    disk->dma_physical_addr = (uint64_t)disk->dma_buffer;
    
    // Initialize cache
    disk->cache_size = DISK_CACHE_BLOCKS;
    disk->cache = (cache_block_t *)(0x2000000 + (disk_count * DISK_CACHE_BLOCKS * sizeof(cache_block_t)));
    memset(disk->cache, 0, disk->cache_size * sizeof(cache_block_t));
    
    // Initialize operation queue
    disk->op_head = 0;
    disk->op_tail = 0;
    
    disk->state = DISK_STATE_INIT;
    
    // Identify device
    uint32_t status;
    if (disk->type == DISK_TYPE_IDE) {
        status = ide_identify_device(disk);
    } else {
        status = ahci_identify_device(disk);
    }
    
    if (status != 0) {
        return status;
    }
    
    // Read partitions
    disk_read_mbr(disk_count);
    
    disk_count++;
    return 0;
}

uint32_t disk_read_sectors(uint32_t disk_id, uint64_t lba, uint32_t count, void *buffer) {
    if (disk_id >= disk_count) {
        return 1;
    }
    
    disk_device_t *disk = &disk_devices[disk_id];
    
    if (disk->state != DISK_STATE_READY) {
        return 2;
    }
    
    acquire_disk_lock();
    
    uint32_t status;
    if (disk->type == DISK_TYPE_IDE) {
        status = ide_read_sectors_pio(disk, lba, count, buffer);
    } else {
        status = ahci_send_cmd(disk, ATA_CMD_READ_DMA_EXT, lba, count, buffer);
    }
    
    release_disk_lock();
    return status;
}

uint32_t disk_write_sectors(uint32_t disk_id, uint64_t lba, uint32_t count, void *buffer) {
    if (disk_id >= disk_count) {
        return 1;
    }
    
    disk_device_t *disk = &disk_devices[disk_id];
    
    if (disk->state != DISK_STATE_READY) {
        return 2;
    }
    
    acquire_disk_lock();
    
    uint32_t status;
    if (disk->type == DISK_TYPE_IDE) {
        status = ide_write_sectors_pio(disk, lba, count, buffer);
    } else {
        status = ahci_send_cmd(disk, ATA_CMD_WRITE_DMA_EXT, lba, count, buffer);
    }
    
    release_disk_lock();
    return status;
}

uint32_t disk_flush_cache(uint32_t disk_id) {
    if (disk_id >= disk_count) {
        return 1;
    }
    
    disk_device_t *disk = &disk_devices[disk_id];
    acquire_disk_lock();
    
    uint32_t status;
    if (disk->type == DISK_TYPE_IDE) {
        outb(disk->cmd_base + IDE_STATUS_CMD, ATA_CMD_FLUSH_CACHE_EXT);
        status = ide_wait_status(disk->cmd_base, DISK_TIMEOUT_MS);
    } else {
        status = ahci_send_cmd(disk, ATA_CMD_FLUSH_CACHE_EXT, 0, 0, NULL);
    }
    
    release_disk_lock();
    return status;
}

uint32_t disk_read_mbr(uint32_t disk_id) {
    if (disk_id >= disk_count) {
        return 1;
    }
    
    uint8_t mbr_buffer[DISK_SECTOR_SIZE];
    if (disk_read_sectors(disk_id, 0, 1, mbr_buffer) != 0) {
        return 1;
    }
    
    // Check MBR signature
    if (mbr_buffer[510] != 0x55 || mbr_buffer[511] != 0xAA) {
        return 2;  // Invalid MBR
    }
    
    return disk_parse_partitions(disk_id);
}

uint32_t disk_parse_partitions(uint32_t disk_id) {
    if (disk_id >= disk_count) {
        return 1;
    }
    
    disk_device_t *disk = &disk_devices[disk_id];
    uint8_t mbr_buffer[DISK_SECTOR_SIZE];
    
    if (disk_read_sectors(disk_id, 0, 1, mbr_buffer) != 0) {
        return 1;
    }
    
    disk->partition_count = 0;
    
    // Parse MBR partition table
    for (int i = 0; i < MAX_DISK_PARTITIONS && i < 4; i++) {
        uint8_t *entry = &mbr_buffer[446 + i * 16];
        
        if (entry[4] == PART_TYPE_EMPTY) {
            continue;
        }
        
        disk_partition_t *part = &disk->partitions[disk->partition_count];
        part->id = disk->partition_count;
        part->type = entry[4];
        part->flags = entry[0];
        
        // Parse CHS to LBA
        part->lba_start = *(uint32_t *)&entry[8];
        part->lba_size = *(uint32_t *)&entry[12];
        
        disk->partition_count++;
    }
    
    return 0;
}

uint32_t disk_get_device_info(uint32_t disk_id, disk_device_t *info) {
    if (disk_id >= disk_count) {
        return 1;
    }
    
    memcpy(info, &disk_devices[disk_id], sizeof(disk_device_t));
    return 0;
}

uint32_t disk_get_stats(uint32_t disk_id, uint64_t *reads, uint64_t *writes, uint64_t *errors) {
    if (disk_id >= disk_count) {
        return 1;
    }
    
    disk_device_t *disk = &disk_devices[disk_id];
    *reads = disk->total_reads;
    *writes = disk->total_writes;
    *errors = disk->total_errors;
    
    return 0;
}

void disk_print_stats(uint32_t disk_id) {
    if (disk_id >= disk_count) {
        return;
    }
    
    disk_device_t *disk = &disk_devices[disk_id];
    
    // TODO: implement printing with serial output
}

// Stub functions for future implementation
uint32_t disk_read_async(uint32_t disk_id, uint64_t lba, uint32_t count, void *buffer) {
    return disk_read_sectors(disk_id, lba, count, buffer);
}

uint32_t disk_write_async(uint32_t disk_id, uint64_t lba, uint32_t count, void *buffer) {
    return disk_write_sectors(disk_id, lba, count, buffer);
}

uint32_t disk_wait_operation(uint32_t disk_id, uint32_t op_id) {
    return 0;
}

uint32_t disk_trim(uint32_t disk_id, uint64_t lba, uint32_t count) {
    return 0;
}

uint32_t disk_cache_read(uint32_t disk_id, uint64_t lba, void *buffer) {
    return 0;
}

uint32_t disk_cache_write(uint32_t disk_id, uint64_t lba, void *buffer) {
    return 0;
}

uint32_t disk_cache_invalidate(uint32_t disk_id, uint64_t lba) {
    return 0;
}

uint32_t disk_cache_flush(uint32_t disk_id) {
    return 0;
}

uint32_t disk_read_smart_data(uint32_t disk_id, void *buffer) {
    return 0;
}

uint32_t disk_get_error_log(uint32_t disk_id, void *buffer) {
    return 0;
}

uint32_t disk_reset_device(uint32_t disk_id) {
    return 0;
}

uint32_t disk_self_test(uint32_t disk_id) {
    return 0;
}

uint32_t disk_set_power_mode(uint32_t disk_id, uint32_t mode) {
    return 0;
}

void disk_irq_handler(uint32_t irq) {
    disk_irq_status |= (1 << irq);
}

void disk_irq_register(uint32_t disk_id, uint32_t irq) {
    if (disk_id >= disk_count) {
        return;
    }
    // TODO: Register IRQ handler
}
