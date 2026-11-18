/** @file
  Head file for BDS Platform specific code

  Copyright (C) 2015-2016, Red Hat, Inc.
  Copyright (c) 2004 - 2008, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2016, Linaro Ltd. All rights reserved.<BR>

  This program and the accompanying materials are licensed and made available
  under the terms and conditions of the BSD License which accompanies this
  distribution. The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS, WITHOUT
  WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#ifndef _PLATFORM_BM_H_
#define _PLATFORM_BM_H_

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

/**
  Use SystemTable Conout to stop video based Simple Text Out consoles from
  going to the video device. Put up LogoFile on every video device that is a
  console.

  @param[in]  LogoFile   File name of logo to display on the center of the
                         screen.

  @retval EFI_SUCCESS     ConsoleControl has been flipped to graphics and logo
                          displayed.
  @retval EFI_UNSUPPORTED Logo not found
**/
EFI_STATUS
EnableQuietBoot (
  IN  EFI_GUID  *LogoFile
  );

/**
  Use SystemTable Conout to turn on video based Simple Text Out consoles. The
  Simple Text Out screens will now be synced up with all non video output
  devices

  @retval EFI_SUCCESS     UGA devices are back in text mode and synced up.
**/
EFI_STATUS
DisableQuietBoot (
  VOID
  );

#ifdef SWITCH_WIFI_RUNTIME
/**
  Load and start a preparation application, then patch the ACPI marker that
  publishes its device (WifiHandoff.c).

  @param[in]  Volume    Handle of the volume that holds FileName.
  @param[in]  FileName  Path of the application, from the volume root.
  @param[in]  Name      Four-character ACPI name whose marker is patched.

  @retval EFI_SUCCESS  The application ran and the marker was patched.
**/
EFI_STATUS
EFIAPI
PrepareWindowsDevice (
  IN EFI_HANDLE  Volume,
  IN CHAR16      *FileName,
  IN CONST CHAR8 Name[4]
  );

/**
  Look for FileName on every simple file system volume and prepare it once.

  @param[in]  FileName  Path of the application, from a volume root.
  @param[in]  Name      Four-character ACPI name whose marker is patched.

  @retval EFI_SUCCESS    The application was found and its device published.
  @retval EFI_NOT_FOUND  No volume holds FileName.
**/
EFI_STATUS
EFIAPI
PrepareWindowsDeviceFromVolumes (
  IN CHAR16       *FileName,
  IN CONST CHAR8  Name[4]
  );
#endif

#endif // _PLATFORM_BM_H_
