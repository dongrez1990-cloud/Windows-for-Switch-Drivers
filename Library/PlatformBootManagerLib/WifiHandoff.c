/** @file
  Windows Wi-Fi handoff: run WifiPrepare.efi, then publish the ACPI device.

  The Lite Home boot path (LiteHomeBoot.c) prepares devices while it is already
  starting bootmgfw.efi from the Windows volume. An Erista build boots through the
  normal BDS boot options instead, so the same preparation has to happen from
  PlatformBootManagerAfterConsole: find WifiPrepare.efi, run it, and only if it
  returned success patch the DSDT byte that makes the WFI0 device visible.

  The ACPI device is defined once, in AcpiTables/Dsdt/Dsdt.asl under
  SWITCH_WIFI_RUNTIME:

      Name (WSTA, 0x10) // BytePrefix marker patched to 0x0F by Home BDS.

  so the patch is a byte search for the NameOp + NameSeg "WSTA" + BytePrefix 0x0A
  + 0x10 sequence, and the byte after 0x0A is set to 0x0F. The DSDT checksum is
  repaired afterwards because Windows validates it on load. Until the byte is
  patched, _STA returns 0 and an unpowered PCIe host is never handed to a driver.

  Copyright (c) 2026, NintendoSwitchPkg contributors. SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "PlatformBm.h"

#ifdef SWITCH_WIFI_RUNTIME
#include <Guid/Acpi.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleFileSystem.h>

/**
  Load and start a preparation application, then mark its ACPI device present.

  @param[in]  Volume    Handle of the volume that holds FileName.
  @param[in]  FileName  Path of the application, from the volume root.
  @param[in]  Name      Four-character ACPI name whose marker is patched.

  @retval EFI_SUCCESS  The application ran and the marker was patched.
  @retval others       The device stays hidden; the caller may continue booting.
**/
EFI_STATUS
EFIAPI
PrepareWindowsDevice (
  IN EFI_HANDLE  Volume,
  IN CHAR16      *FileName,
  IN CONST CHAR8 Name[4]
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt;
  EFI_ACPI_DESCRIPTION_HEADER                   *Table;
  EFI_ACPI_DESCRIPTION_HEADER                   *Dsdt;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE     *Fadt;
  UINT8                                          Pattern[] = { 0x08, 0, 0, 0, 0, 0x0A, 0x10 };
  UINT8                                         *Ready;
  UINT8                                         *Aml;
  UINT8                                          Sum;
  UINTN                                          Entries;
  UINTN                                          Index;
  UINTN                                          Matches;
  EFI_DEVICE_PATH_PROTOCOL                      *Path;
  EFI_HANDLE                                     Image;
  EFI_STATUS                                     Status;

  Dsdt    = NULL;
  Ready   = NULL;
  Matches = 0;

  CopyMem (Pattern + 1, Name, 4);
  Status = EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (VOID **)&Rsdp);
  if (EFI_ERROR (Status) || (Rsdp == NULL) || (Rsdp->XsdtAddress == 0)) {
    return EFI_NOT_FOUND;
  }

  Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  if (Xsdt->Length < sizeof (*Xsdt)) {
    return EFI_COMPROMISED_DATA;
  }

  Entries = (Xsdt->Length - sizeof (*Xsdt)) / sizeof (UINT64);
  for (Index = 0; (Index < Entries) && (Dsdt == NULL); ++Index) {
    Table = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)ReadUnaligned64 (
                                                 (UINT64 *)((UINT8 *)Xsdt + sizeof (*Xsdt) + Index * 8));
    if (Table->Signature != EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
      continue;
    }

    if (Table->Length < sizeof (*Fadt)) {
      return EFI_COMPROMISED_DATA;
    }

    Fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)Table;
    Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(Fadt->XDsdt ? Fadt->XDsdt : Fadt->Dsdt);
  }

  if ((Dsdt == NULL) || (Dsdt->Length < sizeof (*Dsdt))) {
    return EFI_NOT_FOUND;
  }

  Aml = (UINT8 *)Dsdt;
  for (Index = sizeof (*Dsdt); Index + sizeof (Pattern) <= Dsdt->Length; ++Index) {
    if (CompareMem (Aml + Index, Pattern, sizeof (Pattern)) == 0) {
      Ready = Aml + Index + 6;
      Matches++;
    }
  }

  // Exactly one marker, or the patched byte could belong to another device.
  if (Matches != 1) {
    return EFI_NOT_FOUND;
  }

  Path = FileDevicePath (Volume, FileName);
  if (Path == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LoadImage (FALSE, gImageHandle, Path, NULL, 0, &Image);
  FreePool (Path);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->StartImage (Image, NULL, NULL);
  gBS->UnloadImage (Image);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Ready       = 0x0F;
  Dsdt->Checksum = 0;
  for (Index = 0, Sum = 0; Index < Dsdt->Length; ++Index) {
    Sum = (UINT8)(Sum + Aml[Index]);
  }

  Dsdt->Checksum = (UINT8)(0 - Sum);
  MemoryFence ();
  return EFI_SUCCESS;
}

/**
  Look for FileName on every simple file system volume and prepare it once.

  @param[in]  FileName  Path of the application, from a volume root.
  @param[in]  Name      Four-character ACPI name whose marker is patched.

  @retval EFI_SUCCESS  The application was found, ran and patched its device.
  @retval EFI_NOT_FOUND  No volume holds FileName.
  @retval others       The preparation itself failed.
**/
EFI_STATUS
EFIAPI
PrepareWindowsDeviceFromVolumes (
  IN CHAR16       *FileName,
  IN CONST CHAR8  Name[4]
  )
{
  EFI_HANDLE                       *Volumes;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_STATUS                        Status;
  UINTN                             Count;
  UINTN                             Index;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Volumes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; ++Index) {
    if (EFI_ERROR (gBS->HandleProtocol (Volumes[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
      continue;
    }

    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    // Only volumes that actually carry the application are prepared; on a
    // Switch SD card that is the FAT32 ESP, which UEFI reads without any helper.
    if (!EFI_ERROR (Root->Open (Root, &File, FileName, EFI_FILE_MODE_READ, 0))) {
      File->Close (File);
      Root->Close (Root);
      Status = PrepareWindowsDevice (Volumes[Index], FileName, Name);
      if (!EFI_ERROR (Status)) {
        break;
      }

      continue;
    }

    Root->Close (Root);
  }

  gBS->FreePool (Volumes);
  return Status;
}
#endif
