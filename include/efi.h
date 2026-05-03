#pragma once

#include "stdint.h"

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
typedef uint64_t UINTN;
typedef uint32_t UINT32;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct _EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct _EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct _EFI_LOADED_IMAGE_PROTOCOL EFI_LOADED_IMAGE_PROTOCOL;
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct _EFI_FILE_INFO EFI_FILE_INFO;
typedef struct _EFI_MEMORY_DESCRIPTOR EFI_MEMORY_DESCRIPTOR;

struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (*HandleProtocol)(
        EFI_HANDLE Handle,
        const EFI_GUID *Protocol,
        void **Interface
    );
    EFI_STATUS (*AllocatePool)(
        UINT32 PoolType,
        UINTN Size,
        void **Buffer
    );
    EFI_STATUS (*GetMemoryMap)(
        UINTN *MemoryMapSize,
        EFI_MEMORY_DESCRIPTOR *MemoryMap,
        UINTN *MapKey,
        UINTN *DescriptorSize,
        UINT32 *DescriptorVersion
    );
    EFI_STATUS (*ExitBootServices)(
        EFI_HANDLE ImageHandle,
        UINTN MapKey
    );
};

typedef EFI_STATUS (*EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle,
    const EFI_GUID *Protocol,
    void **Interface
);

typedef EFI_STATUS (*EFI_ALLOCATE_POOL)(
    uint32_t PoolType,
    UINTN Size,
    void **Buffer
);

typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    uint32_t *DescriptorVersion
);

typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    UINTN MapKey
);

typedef void (*EFI_COPY_MEM)(
    void *Destination,
    const void *Source,
    UINTN Length
);

typedef void (*EFI_SET_MEM)(
    void *Destination,
    UINTN Length,
    uint8_t Value
);

typedef EFI_STATUS (*EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL *This,
    EFI_FILE_PROTOCOL **NewHandle,
    const CHAR16 *FileName,
    uint64_t OpenMode,
    uint64_t Attributes
);

typedef EFI_STATUS (*EFI_FILE_CLOSE)(
    EFI_FILE_PROTOCOL *This
);

typedef EFI_STATUS (*EFI_FILE_READ)(
    EFI_FILE_PROTOCOL *This,
    UINTN *BufferSize,
    void *Buffer
);

typedef EFI_STATUS (*EFI_FILE_GET_INFO)(
    EFI_FILE_PROTOCOL *This,
    const EFI_GUID *InformationType,
    UINTN *BufferSize,
    void *Buffer
);

struct _EFI_FILE_PROTOCOL {
    EFI_TABLE_HEADER Hdr;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    EFI_FILE_READ Read;
    EFI_FILE_GET_INFO GetInfo;
};

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (*OpenVolume)(
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
        EFI_FILE_PROTOCOL **Root
    );
};

struct _EFI_LOADED_IMAGE_PROTOCOL {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_HANDLE DeviceHandle;
    EFI_HANDLE FilePath;
    void *Reserved;
    void *LoadOptions;
    uint32_t LoadOptionsSize;
    void *ImageBase;
    uint64_t ImageSize;
    EFI_STATUS (*Unload)(EFI_HANDLE ImageHandle);
};

struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    void *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    EFI_BOOT_SERVICES *BootServices;
    void *RuntimeServices;
    void *ConfigurationTable;
};

struct _EFI_MEMORY_DESCRIPTOR {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
};

struct _EFI_FILE_INFO {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint64_t CreateTime[4];
    uint64_t LastAccessTime[4];
    uint64_t ModificationTime[4];
    uint64_t Attribute;
    CHAR16 FileName[1];
};

#define EFI_SUCCESS 0ULL
#define EFI_LOAD_ERROR 0x8000000000000001ULL
#define EFI_ERROR(x) (((EFI_STATUS)(x)) != EFI_SUCCESS)
#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EfiLoaderData 2

extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiFileInfoGuid;
