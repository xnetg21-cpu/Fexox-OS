#include "nvme.h"
#include <string.h>

// ============================
// NVME DEVICE POOL
// ============================

#define MAX_NVME_DEVICES 8
nvme_device_t nvme_devices[MAX_NVME_DEVICES];
uint32_t nvme_device_count = 0;
uint32_t nvme_lock = 0;

// ============================
// UTILITY FUNCTIONS
// ============================

static inline void acquire_nvme_lock(void) {
    while (__sync_lock_test_and_set(&nvme_lock, 1)) {
        __asm__ volatile ("pause");
    }
}

static inline void release_nvme_lock(void) {
    __sync_lock_release(&nvme_lock);
}

static void nvme_delay_ms(uint32_t ms) {
    volatile uint32_t counter = ms * 1000;
    while (counter--) {
        __asm__ volatile ("pause");
    }
}

static uint32_t nvme_readl(volatile uint32_t *addr) {
    return *addr;
}

static void nvme_writel(volatile uint32_t *addr, uint32_t value) {
    *addr = value;
}

static uint64_t nvme_readq(volatile uint64_t *addr) {
    return *addr;
}

static void nvme_writeq(volatile uint64_t *addr, uint64_t value) {
    *addr = value;
}

// ============================
// NVME CONTROLLER FUNCTIONS
// ============================

static uint32_t nvme_wait_ready(nvme_device_t *dev, uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    
    while (elapsed < timeout_ms) {
        uint32_t csts = nvme_readl((volatile uint32_t *)&dev->bar->csts);
        
        if (csts & 0x01) {  // RDY bit set
            return 0;
        }
        
        nvme_delay_ms(1);
        elapsed++;
    }
    
    return 1;  // Timeout
}

static uint32_t nvme_reset_controller(nvme_device_t *dev) {
    // Disable controller
    uint32_t cc = nvme_readl((volatile uint32_t *)&dev->bar->cc);
    cc &= ~0x01;  // Clear EN bit
    nvme_writel((volatile uint32_t *)&dev->bar->cc, cc);
    
    // Wait for CSTS.RDY to clear
    uint32_t elapsed = 0;
    while (elapsed < 5000) {
        uint32_t csts = nvme_readl((volatile uint32_t *)&dev->bar->csts);
        if (!(csts & 0x01)) {
            break;
        }
        nvme_delay_ms(1);
        elapsed++;
    }
    
    if (elapsed >= 5000) {
        return 1;  // Failed to disable
    }
    
    // Reset CC register to defaults
    nvme_writel((volatile uint32_t *)&dev->bar->cc, 0);
    
    // Set up Admin Queue Attributes
    uint32_t aqa = ((NVME_SQ_SIZE - 1) << 16) | (NVME_CQ_SIZE - 1);
    nvme_writel((volatile uint32_t *)&dev->bar->aqa, aqa);
    
    // Set Admin Submission Queue Base Address
    nvme_writeq((volatile uint64_t *)&dev->bar->asq, (uint64_t)dev->admin_sq->entries);
    
    // Set Admin Completion Queue Base Address
    nvme_writeq((volatile uint64_t *)&dev->bar->acq, (uint64_t)dev->admin_cq->entries);
    
    // Enable controller with appropriate settings
    cc = 0x00460000;  // CSS=0, SHN=0, AMS=0, MPS=0, CQR=1, EN=1
    nvme_writel((volatile uint32_t *)&dev->bar->cc, cc);
    
    // Wait for controller to be ready
    if (nvme_wait_ready(dev, 5000) != 0) {
        return 2;  // Controller not ready
    }
    
    return 0;
}

static uint32_t nvme_submit_admin_cmd(nvme_device_t *dev, nvme_cmd_t *cmd) {
    // Submit command to admin SQ
    if (dev->admin_sq->tail >= dev->admin_sq->size) {
        return 1;  // Queue full
    }
    
    memcpy(&dev->admin_sq->entries[dev->admin_sq->tail], cmd, sizeof(nvme_cmd_t));
    dev->admin_sq->tail = (dev->admin_sq->tail + 1) % dev->admin_sq->size;
    
    // Ring doorbell
    volatile uint32_t *db = (volatile uint32_t *)((uint64_t)dev->bar + 0x1000);
    nvme_writel(db, dev->admin_sq->tail);
    
    // Wait for completion
    uint32_t elapsed = 0;
    while (elapsed < dev->timeout) {
        if (dev->admin_cq->tail != dev->admin_cq->head) {
            break;
        }
        nvme_delay_ms(1);
        elapsed++;
    }
    
    if (elapsed >= dev->timeout) {
        return 2;  // Timeout
    }
    
    // Check completion status
    nvme_completion_t *cpl = &dev->admin_cq->entries[dev->admin_cq->head];
    uint16_t status = cpl->status & 0xFF;
    
    dev->admin_cq->head = (dev->admin_cq->head + 1) % dev->admin_cq->size;
    
    // Ring completion doorbell
    volatile uint32_t *cq_db = (volatile uint32_t *)((uint64_t)dev->bar + 0x1004);
    nvme_writel(cq_db, dev->admin_cq->head);
    
    return status;
}

static uint32_t nvme_identify_controller(nvme_device_t *dev) {
    if (!dev->ctlr_info) {
        return 1;  // Not allocated
    }
    
    nvme_cmd_t cmd = {0};
    cmd.cdw0 = NVME_ADM_CMD_IDENTIFY;
    cmd.dptr[0] = (uint64_t)dev->ctlr_info;
    cmd.dptr[1] = 0;
    cmd.cdw10 = 0;  // Identify controller
    
    return nvme_submit_admin_cmd(dev, &cmd);
}

static uint32_t nvme_identify_namespace(nvme_device_t *dev, uint32_t nsid) {
    if (nsid > 255 || !dev->ns_info[nsid]) {
        return 1;
    }
    
    nvme_cmd_t cmd = {0};
    cmd.cdw0 = NVME_ADM_CMD_IDENTIFY;
    cmd.dptr[0] = (uint64_t)dev->ns_info[nsid];
    cmd.dptr[1] = 0;
    cmd.cdw10 = 0;  // Identify controller
    cmd.cdw11 = nsid;
    
    return nvme_submit_admin_cmd(dev, &cmd);
}

static uint32_t nvme_create_io_queue(nvme_device_t *dev, uint32_t qid) {
    // Create completion queue first
    nvme_cmd_t cq_cmd = {0};
    cq_cmd.cdw0 = NVME_ADM_CMD_CREATE_CQ;
    cq_cmd.dptr[0] = (uint64_t)dev->io_cqs[qid]->entries;
    cq_cmd.cdw10 = ((NVME_CQ_SIZE - 1) << 16) | qid;
    cq_cmd.cdw11 = 0x01;  // Physically contiguous, interrupts enabled
    
    if (nvme_submit_admin_cmd(dev, &cq_cmd) != 0) {
        return 1;
    }
    
    // Create submission queue
    nvme_cmd_t sq_cmd = {0};
    sq_cmd.cdw0 = NVME_ADM_CMD_CREATE_SQ;
    sq_cmd.dptr[0] = (uint64_t)dev->io_sqs[qid]->entries;
    sq_cmd.cdw10 = ((NVME_SQ_SIZE - 1) << 16) | qid;
    sq_cmd.cdw11 = (qid << 16) | 0x01;  // CQ ID, physically contiguous
    
    return nvme_submit_admin_cmd(dev, &sq_cmd);
}

// ============================
// NVME I/O COMMANDS
// ============================

static uint32_t nvme_read(nvme_device_t *dev, uint32_t nsid, uint64_t lba, uint32_t nlb, void *buffer) {
    if (nsid == 0 || nsid > dev->ns_count) {
        return 1;
    }
    
    nvme_cmd_t cmd = {0};
    cmd.cdw0 = (1 << 8) | NVME_NVM_CMD_READ;  // NVM command set
    cmd.dptr[0] = (uint64_t)buffer;
    cmd.dptr[1] = 0;
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = (lba >> 32) & 0xFFFFFFFF;
    cmd.cdw12 = (nlb - 1) & 0xFFFF;
    
    return nvme_submit_admin_cmd(dev, &cmd);
}

static uint32_t nvme_write(nvme_device_t *dev, uint32_t nsid, uint64_t lba, uint32_t nlb, void *buffer) {
    if (nsid == 0 || nsid > dev->ns_count) {
        return 1;
    }
    
    nvme_cmd_t cmd = {0};
    cmd.cdw0 = (1 << 8) | NVME_NVM_CMD_WRITE;  // NVM command set
    cmd.dptr[0] = (uint64_t)buffer;
    cmd.dptr[1] = 0;
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = (lba >> 32) & 0xFFFFFFFF;
    cmd.cdw12 = (nlb - 1) & 0xFFFF;
    
    return nvme_submit_admin_cmd(dev, &cmd);
}

static uint32_t nvme_flush(nvme_device_t *dev, uint32_t nsid) {
    nvme_cmd_t cmd = {0};
    cmd.cdw0 = (1 << 8) | NVME_NVM_CMD_FLUSH;  // NVM command set
    
    return nvme_submit_admin_cmd(dev, &cmd);
}

// ============================
// PUBLIC INTERFACE
// ============================

uint32_t nvme_probe_device(uint16_t bus, uint16_t device, uint16_t function) {
    if (nvme_device_count >= MAX_NVME_DEVICES) {
        return 1;
    }
    
    nvme_device_t *dev = &nvme_devices[nvme_device_count];
    
    // TODO: Read PCI configuration space
    // dev->pci_vendor_id = pci_read_word(bus, device, function, 0x00);
    // dev->pci_device_id = pci_read_word(bus, device, function, 0x02);
    // dev->bar = (volatile nvme_bar_t *)pci_read_long(bus, device, function, 0x10);
    
    dev->device_id = nvme_device_count;
    dev->max_queue_entries = 0;
    dev->db_stride = 4;  // 4 bytes
    dev->timeout = 5000;
    
    // Allocate memory
    dev->ctlr_info = (nvme_identify_ctlr_t *)0x3000000;
    dev->admin_sq = (nvme_sq_t *)0x3001000;
    dev->admin_cq = (nvme_cq_t *)0x3002000;
    dev->prp_pool = (uint8_t *)0x3003000;
    dev->prp_pool_phys = 0x3003000;
    
    for (int i = 0; i < 64; i++) {
        dev->io_sqs[i] = (nvme_sq_t *)(0x3010000 + i * 0x1000);
        dev->io_cqs[i] = (nvme_cq_t *)(0x3050000 + i * 0x1000);
    }
    
    // Initialize queue structures
    dev->admin_sq->size = NVME_SQ_SIZE;
    dev->admin_sq->entries = (nvme_cmd_t *)(0x3001000 + sizeof(nvme_sq_t));
    dev->admin_sq->head = 0;
    dev->admin_sq->tail = 0;
    
    dev->admin_cq->size = NVME_CQ_SIZE;
    dev->admin_cq->entries = (nvme_completion_t *)(0x3002000 + sizeof(nvme_cq_t));
    dev->admin_cq->head = 0;
    dev->admin_cq->tail = 0;
    dev->admin_cq->phase = 1;
    
    // Reset controller
    if (nvme_reset_controller(dev) != 0) {
        return 2;
    }
    
    // Identify controller
    if (nvme_identify_controller(dev) != 0) {
        return 3;
    }
    
    // Read max queue entries
    dev->max_queue_entries = dev->ctlr_info->sqes & 0x0F;
    
    // Create I/O queue pair
    if (nvme_create_io_queue(dev, 1) != 0) {
        return 4;
    }
    
    // Identify all namespaces
    dev->ns_count = dev->ctlr_info->nn;
    if (dev->ns_count > 256) {
        dev->ns_count = 256;
    }
    
    for (uint32_t i = 0; i < dev->ns_count; i++) {
        dev->ns_info[i] = (nvme_identify_ns_t *)(0x3080000 + i * 0x1000);
        
        if (nvme_identify_namespace(dev, i + 1) == 0) {
            // Calculate total sectors
            dev->total_sectors += dev->ns_info[i]->nsze;
        }
    }
    
    nvme_device_count++;
    return 0;
}

uint32_t nvme_init(void) {
    // Initialize NVME subsystem
    memset(nvme_devices, 0, sizeof(nvme_devices));
    nvme_device_count = 0;
    
    // TODO: Scan PCI bus for NVMe controllers
    
    return 0;
}

uint32_t nvme_read_sectors(uint32_t device_id, uint64_t lba, uint32_t count, void *buffer) {
    if (device_id >= nvme_device_count) {
        return 1;
    }
    
    nvme_device_t *dev = &nvme_devices[device_id];
    
    acquire_nvme_lock();
    uint32_t status = nvme_read(dev, 1, lba, count, buffer);
    release_nvme_lock();
    
    if (status == 0) {
        dev->stats_reads += count;
    } else {
        dev->stats_errors++;
    }
    
    return status;
}

uint32_t nvme_write_sectors(uint32_t device_id, uint64_t lba, uint32_t count, void *buffer) {
    if (device_id >= nvme_device_count) {
        return 1;
    }
    
    nvme_device_t *dev = &nvme_devices[device_id];
    
    acquire_nvme_lock();
    uint32_t status = nvme_write(dev, 1, lba, count, buffer);
    release_nvme_lock();
    
    if (status == 0) {
        dev->stats_writes += count;
    } else {
        dev->stats_errors++;
    }
    
    return status;
}

uint32_t nvme_flush_cache(uint32_t device_id) {
    if (device_id >= nvme_device_count) {
        return 1;
    }
    
    nvme_device_t *dev = &nvme_devices[device_id];
    
    acquire_nvme_lock();
    uint32_t status = nvme_flush(dev, 1);
    release_nvme_lock();
    
    return status;
}

uint32_t nvme_get_stats(uint32_t device_id, uint64_t *reads, uint64_t *writes, uint64_t *errors) {
    if (device_id >= nvme_device_count) {
        return 1;
    }
    
    nvme_device_t *dev = &nvme_devices[device_id];
    *reads = dev->stats_reads;
    *writes = dev->stats_writes;
    *errors = dev->stats_errors;
    
    return 0;
}
