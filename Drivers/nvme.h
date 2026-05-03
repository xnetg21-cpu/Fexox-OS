#pragma once

#include <stdint.h>

// ============================
// NVME REGISTER DEFINITIONS
// ============================

// NVME Controller Registers
#define NVME_REG_CAP 0x0000    // Controller Capabilities
#define NVME_REG_VS 0x0008     // Version
#define NVME_REG_INTMS 0x000C  // Interrupt Mask Set
#define NVME_REG_INTMC 0x0010  // Interrupt Mask Clear
#define NVME_REG_CC 0x0014     // Controller Configuration
#define NVME_REG_CSTS 0x001C   // Controller Status
#define NVME_REG_NSSR 0x0020   // NVM Subsystem Reset
#define NVME_REG_AQA 0x0024    // Admin Queue Attributes
#define NVME_REG_ASQ 0x0028    // Admin Submission Queue Base
#define NVME_REG_ACQ 0x0030    // Admin Completion Queue Base
#define NVME_REG_CMBLOC 0x0038 // Controller Memory Buffer Location
#define NVME_REG_CMBSZ 0x003C  // Controller Memory Buffer Size
#define NVME_REG_BPINFO 0x0040 // Boot Partition Information
#define NVME_REG_BPRSEL 0x0044 // Boot Partition Select
#define NVME_REG_BPMBL 0x0048  // Boot Partition Memory Buffer Location
#define NVME_REG_CMBMSC 0x0050 // Controller Memory Buffer Memory Space Control
#define NVME_REG_CMBSTS 0x0058 // Controller Memory Buffer Status
#define NVME_REG_CMBEBS 0x005C // Controller Memory Buffer Elasticity Buffer Size
#define NVME_REG_CMBSWTP 0x0060 // Controller Memory Buffer Sustained Write Throughput
#define NVME_REG_NSSD 0x0064   // NVM Subsystem Shutdown
#define NVME_REG_CRTO 0x0068   // Controller Reset Timeout
#define NVME_REG_SQ0TDPL 0x1000 // SQ0 Tail Doorbell

// NVME Status Codes
#define NVME_STATUS_SUCCESS 0x0
#define NVME_STATUS_INVALID_CMD 0x1
#define NVME_STATUS_INVALID_FIELD 0x2
#define NVME_STATUS_CMD_ID_CONFLICT 0x3
#define NVME_STATUS_DATA_XFER_ERROR 0x4
#define NVME_STATUS_CMD_ABORT 0x5

// NVME Admin Commands
#define NVME_ADM_CMD_DELETE_SQ 0x00
#define NVME_ADM_CMD_CREATE_SQ 0x01
#define NVME_ADM_CMD_GET_LOG_PAGE 0x02
#define NVME_ADM_CMD_DELETE_CQ 0x04
#define NVME_ADM_CMD_CREATE_CQ 0x05
#define NVME_ADM_CMD_IDENTIFY 0x06
#define NVME_ADM_CMD_ABORT_CMD 0x08
#define NVME_ADM_CMD_SET_FEATURES 0x09
#define NVME_ADM_CMD_GET_FEATURES 0x0A
#define NVME_ADM_CMD_ASYNC_EVENT 0x0C
#define NVME_ADM_CMD_FW_COMMIT 0x10
#define NVME_ADM_CMD_FW_DOWNLOAD 0x11
#define NVME_ADM_CMD_DEV_SELF_TEST 0x14
#define NVME_ADM_CMD_NAMESPACE_MGMT 0x20
#define NVME_ADM_CMD_FIRMWARE_ACTIVATE 0x10
#define NVME_ADM_CMD_FORMAT_NVM 0x80
#define NVME_ADM_CMD_SECURITY_SEND 0x81
#define NVME_ADM_CMD_SECURITY_RECV 0x82

// NVME NVM Commands
#define NVME_NVM_CMD_FLUSH 0x00
#define NVME_NVM_CMD_WRITE 0x01
#define NVME_NVM_CMD_READ 0x02
#define NVME_NVM_CMD_WRITE_UNCOR 0x04
#define NVME_NVM_CMD_COMPARE 0x05
#define NVME_NVM_CMD_WRITE_ZEROS 0x08
#define NVME_NVM_CMD_DATASET_MGMT 0x09
#define NVME_NVM_CMD_VERIFY 0x0C
#define NVME_NVM_CMD_COPY 0x19

// Queue sizes
#define NVME_SQ_SIZE 64
#define NVME_CQ_SIZE 64

// ============================
// NVME DATA STRUCTURES
// ============================

typedef struct {
    uint64_t cap;       // Controller Capabilities
    uint32_t vs;        // Version
    uint32_t intms;     // Interrupt Mask Set
    uint32_t intmc;     // Interrupt Mask Clear
    uint32_t cc;        // Controller Configuration
    uint32_t csts;      // Controller Status
    uint32_t nssr;      // NVM Subsystem Reset
    uint32_t aqa;       // Admin Queue Attributes
    uint64_t asq;       // Admin Submission Queue Base
    uint64_t acq;       // Admin Completion Queue Base
} nvme_bar_t;

typedef struct {
    uint32_t cdw0;
    uint32_t reserved1;
    uint64_t mptr;
    uint64_t dptr[2];
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_cmd_t;

typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sqhd;
    uint16_t sqid;
    uint16_t cid;
    uint16_t status;
} nvme_completion_t;

typedef struct {
    uint16_t vid;
    uint16_t ssvid;
    uint8_t sn[20];
    uint8_t mn[40];
    uint8_t fr[8];
    uint8_t rab;
    uint8_t ieee[3];
    uint8_t cmic;
    uint8_t mdts;
    uint8_t cntlid[2];
    uint32_t ver;
    uint32_t rtd3r;
    uint32_t rtd3e;
    uint32_t oaes;
    uint32_t ctratt;
    uint8_t reserved1[12];
    uint8_t fguid[16];
    uint8_t reserved2[128];
    uint16_t oacs;
    uint8_t acl;
    uint8_t aerl;
    uint8_t fwug;
    uint8_t lpa;
    uint8_t elpe;
    uint8_t npss;
    uint8_t avscc;
    uint8_t apsta;
    uint16_t wctemp;
    uint16_t cctemp;
    uint16_t mtfa;
    uint32_t hmpre;
    uint32_t hmmin;
    uint8_t tnvmcap[16];
    uint8_t unvmcap[16];
    uint32_t rpmbs;
    uint16_t edstt;
    uint8_t dsto;
    uint8_t fwug2;
    uint16_t ctrlr_busy_time[100];
    uint8_t reserved3[270];
    uint8_t sqes;
    uint8_t cqes;
    uint16_t maxcmd;
    uint32_t nn;
    uint16_t oncs;
    uint16_t fuses;
    uint8_t fna;
    uint8_t vwc;
    uint16_t awun;
    uint16_t awupf;
    uint8_t icsvscc;
    uint8_t nwpc;
    uint16_t acwu;
    uint8_t reserved4[2];
    uint32_t sgls;
    uint8_t reserved5[184];
} nvme_identify_ctlr_t;

typedef struct {
    uint64_t nsze;
    uint64_t ncap;
    uint64_t nuse;
    uint8_t nsfeat;
    uint8_t nlbaf;
    uint8_t flbas;
    uint8_t mc;
    uint8_t dpc;
    uint8_t dps;
    uint8_t nprg;
    uint8_t rsvd33;
    uint8_t fpi;
    uint8_t rsvd35;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacon;
    uint16_t nacwu;
    uint16_t nabsn;
    uint16_t nabo;
    uint16_t nabspf;
    uint16_t rsvd54;
    uint8_t nvmcap[16];
    uint8_t rsvd72[40];
    uint8_t nguid[16];
    uint8_t eui64[8];
    uint32_t lbaf[64];
    uint8_t rsvd384[3712];
} nvme_identify_ns_t;

// ============================
// NVME QUEUE STRUCTURES
// ============================

typedef struct {
    uint16_t head;
    uint16_t tail;
    uint16_t size;
    nvme_cmd_t *entries;
} nvme_sq_t;

typedef struct {
    uint16_t head;
    uint16_t tail;
    uint16_t size;
    uint16_t phase;
    nvme_completion_t *entries;
} nvme_cq_t;

// ============================
// NVME DEVICE STRUCTURE
// ============================

typedef struct {
    uint32_t device_id;
    uint16_t pci_vendor_id;
    uint16_t pci_device_id;
    uint16_t pci_subsystem_vendor;
    uint16_t pci_subsystem_device;
    
    // BAR and memory
    volatile nvme_bar_t *bar;
    uint32_t bar_size;
    
    // Controller info
    nvme_identify_ctlr_t *ctlr_info;
    uint32_t max_queue_entries;
    uint32_t db_stride;
    uint32_t timeout;
    
    // Queues
    nvme_sq_t *admin_sq;
    nvme_cq_t *admin_cq;
    nvme_sq_t *io_sqs[64];
    nvme_cq_t *io_cqs[64];
    
    // Namespaces
    nvme_identify_ns_t *ns_info[256];
    uint32_t ns_count;
    uint64_t total_sectors;
    
    // DMA buffers
    uint8_t *prp_pool;
    uint64_t prp_pool_phys;
    
    // State
    uint32_t state;
    uint64_t stats_reads;
    uint64_t stats_writes;
    uint64_t stats_errors;
} nvme_device_t;

#endif // NVME_HEADER
