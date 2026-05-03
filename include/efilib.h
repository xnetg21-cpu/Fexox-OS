#pragma once

#include "efi.h"
#include "string.h"

#define uefi_call_wrapper(Function, NumberOfArgs, ...) ((Function)(__VA_ARGS__))
#define SetMem(Destination, Length, Value) memset((Destination), (Value), (Length))
#define CopyMem(Destination, Source, Length) memcpy((Destination), (Source), (Length))

void InitializeLib(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
void Print(const CHAR16 *Format, ...);
