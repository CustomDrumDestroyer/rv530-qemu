/** @file
  Minimal GOP for the emulated ATI Radeon X1600 (rv530).  See the .inf.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/PciIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/BaseLib.h>

#define RV_VENDOR  0x1002
#define RV_DEVICE  0x71C5
#define RV_DEVICE2 0x71C2

/* AVIVO D1 registers (Linux r500_reg.h) */
#define D1GRPH_ENABLE                  0x6100
#define D1GRPH_CONTROL                 0x6104
#define D1GRPH_PRIMARY_SURFACE_ADDRESS 0x6110
#define D1GRPH_PITCH                   0x6120
#define D1GRPH_X_END                   0x6134
#define D1GRPH_Y_END                   0x6138
#define D1CRTC_CONTROL                 0x6080

typedef struct {
  UINT64                                   Signature;
  EFI_HANDLE                               Handle;
  EFI_PCI_IO_PROTOCOL                      *PciIo;
  UINT8                                    MmioBar;   /* PciIo BAR index of the 64 KiB registers */
  UINT8                                    VramBar;   /* PciIo BAR index of the VRAM aperture */
  UINT64                                   OriginalAttributes;
  EFI_GRAPHICS_OUTPUT_PROTOCOL             Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE        Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION     Info;
  EFI_DEVICE_PATH_PROTOCOL                 *DevicePath;
  FRAME_BUFFER_CONFIGURE                   *BltConfig;
  UINTN                                    BltConfigSize;
} RV530_GOP;

#define RV530_GOP_SIG  SIGNATURE_64 ('R','V','5','3','0','G','O','P')
#define FROM_GOP(a)    CR (a, RV530_GOP, Gop, RV530_GOP_SIG)

STATIC
UINT32
RvRead (
  IN RV530_GOP  *Rv,
  IN UINT32     Offset
  )
{
  UINT32  Value = 0;

  Rv->PciIo->Mem.Read (Rv->PciIo, EfiPciIoWidthUint32, Rv->MmioBar, Offset, 1, &Value);
  return Value;
}

STATIC
EFI_STATUS
EFIAPI
RvQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  RV530_GOP  *Rv = FROM_GOP (This);

  if ((ModeNumber != 0) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocateCopyPool (sizeof (Rv->Info), &Rv->Info);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (Rv->Info);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RvSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  RV530_GOP                      *Rv = FROM_GOP (This);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;
  EFI_STATUS                     Status;

  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  if (Rv->BltConfig == NULL) {
    Rv->BltConfigSize = 0;
    Status = FrameBufferBltConfigure ((VOID *)(UINTN)Rv->Mode.FrameBufferBase,
               &Rv->Info, Rv->BltConfig, &Rv->BltConfigSize);
    if (Status == RETURN_BUFFER_TOO_SMALL) {
      Rv->BltConfig = AllocatePool (Rv->BltConfigSize);
      if (Rv->BltConfig == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      Status = FrameBufferBltConfigure ((VOID *)(UINTN)Rv->Mode.FrameBufferBase,
                 &Rv->Info, Rv->BltConfig, &Rv->BltConfigSize);
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  ZeroMem (&Black, sizeof (Black));
  FrameBufferBlt (Rv->BltConfig, &Black, EfiBltVideoFill, 0, 0, 0, 0,
    Rv->Info.HorizontalResolution, Rv->Info.VerticalResolution, 0);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RvBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer  OPTIONAL,
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN  UINTN                              SourceX,
  IN  UINTN                              SourceY,
  IN  UINTN                              DestinationX,
  IN  UINTN                              DestinationY,
  IN  UINTN                              Width,
  IN  UINTN                              Height,
  IN  UINTN                              Delta
  )
{
  RV530_GOP  *Rv = FROM_GOP (This);

  if (Rv->BltConfig == NULL) {
    return EFI_NOT_READY;
  }

  return FrameBufferBlt (Rv->BltConfig, BltBuffer, BltOperation, SourceX,
           SourceY, DestinationX, DestinationY, Width, Height, Delta);
}

STATIC
EFI_STATUS
EFIAPI
RvSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT16               Ids[2];

  Status = gBS->OpenProtocol (Controller, &gEfiPciIoProtocolGuid, (VOID **)&PciIo,
                  This->DriverBindingHandle, Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, 0, 2, Ids);
  if (!EFI_ERROR (Status) &&
      ((Ids[0] != RV_VENDOR) || ((Ids[1] != RV_DEVICE) && (Ids[1] != RV_DEVICE2)))) {
    Status = EFI_UNSUPPORTED;
  }

  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RvStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                         Status;
  RV530_GOP                          *Rv;
  EFI_DEVICE_PATH_PROTOCOL           *ParentPath;
  ACPI_ADR_DEVICE_PATH               AcpiAdr;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Bar0;
  UINT64                             Supports;
  UINT32                             Width, Height, Pitch;
  EFI_PCI_IO_PROTOCOL                *ChildPciIo;

  Rv = AllocateZeroPool (sizeof (*Rv));
  if (Rv == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Rv->Signature = RV530_GOP_SIG;

  Status = gBS->OpenProtocol (Controller, &gEfiPciIoProtocolGuid, (VOID **)&Rv->PciIo,
                  This->DriverBindingHandle, Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    FreePool (Rv);
    return Status;
  }

  Rv->PciIo->Attributes (Rv->PciIo, EfiPciIoAttributeOperationGet, 0, &Rv->OriginalAttributes);
  Rv->PciIo->Attributes (Rv->PciIo, EfiPciIoAttributeOperationSupported, 0, &Supports);
  Status = Rv->PciIo->Attributes (Rv->PciIo, EfiPciIoAttributeOperationEnable,
             (Supports & EFI_PCI_DEVICE_ENABLE) | EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY, NULL);
  if (EFI_ERROR (Status)) {
    Status = Rv->PciIo->Attributes (Rv->PciIo, EfiPciIoAttributeOperationEnable,
               Supports & EFI_PCI_DEVICE_ENABLE, NULL);
  }
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  /*
   * EDK2 numbers BARs sequentially, not by config-space slot: the 64-bit
   * VRAM BAR (slots 0-1) is index 0 and the register BAR (slot 2) is index 1.
   * Identify them by size instead of assuming.
   */
  {
    UINT8                              Index;
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Desc;

    Rv->MmioBar = 0xFF;
    Rv->VramBar = 0xFF;
    for (Index = 0; Index < 6; Index++) {
      if (EFI_ERROR (Rv->PciIo->GetBarAttributes (Rv->PciIo, Index, NULL, (VOID **)&Desc))) {
        continue;
      }
      if (Desc->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) {
        if ((Desc->AddrLen == SIZE_64KB) && (Rv->MmioBar == 0xFF)) {
          Rv->MmioBar = Index;
        } else if ((Desc->AddrLen >= SIZE_16MB) && (Rv->VramBar == 0xFF)) {
          Rv->VramBar = Index;
        }
      }
      FreePool (Desc);
    }
    if ((Rv->MmioBar == 0xFF) || (Rv->VramBar == 0xFF)) {
      Status = EFI_DEVICE_ERROR;
      goto Fail;
    }
  }

  Status = Rv->PciIo->GetBarAttributes (Rv->PciIo, Rv->VramBar, NULL, (VOID **)&Bar0);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  /* The posted mode set by the device at reset (xres/yres properties). */
  Width  = RvRead (Rv, D1GRPH_X_END);
  Height = RvRead (Rv, D1GRPH_Y_END);
  Pitch  = RvRead (Rv, D1GRPH_PITCH) & 0x3FFF;
  if ((Width == 0) || (Height == 0) || (Width > 4096) || (Height > 4096)) {
    Width = 1024; Height = 768;
  }
  if (Pitch < Width) {
    Pitch = Width;
  }
  DEBUG ((DEBUG_INFO, "Rv530Gop: %ux%u pitch %u fb 0x%lx surf 0x%x\n", Width, Height,
    Pitch, Bar0->AddrRangeMin, RvRead (Rv, D1GRPH_PRIMARY_SURFACE_ADDRESS)));

  Rv->Info.Version              = 0;
  Rv->Info.HorizontalResolution = Width;
  Rv->Info.VerticalResolution   = Height;
  Rv->Info.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  Rv->Info.PixelsPerScanLine    = Pitch;
  Rv->Mode.MaxMode              = 1;
  Rv->Mode.Mode                 = 0;
  Rv->Mode.Info                 = &Rv->Info;
  Rv->Mode.SizeOfInfo           = sizeof (Rv->Info);
  /* the D1 surface is at VRAM offset PRIMARY_SURFACE_ADDRESS (0 at reset) */
  Rv->Mode.FrameBufferBase      = Bar0->AddrRangeMin + RvRead (Rv, D1GRPH_PRIMARY_SURFACE_ADDRESS);
  Rv->Mode.FrameBufferSize      = (UINTN)Pitch * Height * 4;
  FreePool (Bar0);

  Rv->Gop.QueryMode = RvQueryMode;
  Rv->Gop.SetMode   = RvSetMode;
  Rv->Gop.Blt       = RvBlt;
  Rv->Gop.Mode      = &Rv->Mode;

  Status = RvSetMode (&Rv->Gop, 0);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  /* child handle: parent device path + ACPI _ADR of the display */
  Status = gBS->HandleProtocol (Controller, &gEfiDevicePathProtocolGuid, (VOID **)&ParentPath);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
  ZeroMem (&AcpiAdr, sizeof (AcpiAdr));
  AcpiAdr.Header.Type    = ACPI_DEVICE_PATH;
  AcpiAdr.Header.SubType = ACPI_ADR_DP;
  AcpiAdr.ADR            = ACPI_DISPLAY_ADR (1, 0, 0, 1, 0, ACPI_ADR_DISPLAY_TYPE_VGA, 0, 0);
  SetDevicePathNodeLength (&AcpiAdr.Header, sizeof (AcpiAdr));
  Rv->DevicePath = AppendDevicePathNode (ParentPath, (EFI_DEVICE_PATH_PROTOCOL *)&AcpiAdr);
  if (Rv->DevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (&Rv->Handle,
                  &gEfiDevicePathProtocolGuid, Rv->DevicePath,
                  &gEfiGraphicsOutputProtocolGuid, &Rv->Gop, NULL);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  gBS->OpenProtocol (Controller, &gEfiPciIoProtocolGuid, (VOID **)&ChildPciIo,
    This->DriverBindingHandle, Rv->Handle, EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER);
  return EFI_SUCCESS;

Fail:
  Rv->PciIo->Attributes (Rv->PciIo, EfiPciIoAttributeOperationSet, Rv->OriginalAttributes, NULL);
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  FreePool (Rv);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RvStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  /* The GOP stays for the lifetime of boot services. */
  return EFI_UNSUPPORTED;
}

STATIC EFI_DRIVER_BINDING_PROTOCOL  mRvBinding = {
  RvSupported, RvStart, RvStop, 0x10, NULL, NULL
};

EFI_STATUS
EFIAPI
Rv530GopEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBinding (ImageHandle, SystemTable, &mRvBinding, ImageHandle);
}
