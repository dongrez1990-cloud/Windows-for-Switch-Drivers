/* SPDX-License-Identifier: GPL-2.0-only */
/* Checkpoint log for the Wi-Fi preparation path.
 *
 * The firmware's exception handler can only draw a register dump on the panel
 * and halt, which a photograph cannot resolve into a call site. This writes the
 * progress of each step to \SwitchWOA\Wifi\wifiprep.log on the first volume
 * that accepts it, flushing after every line: whatever the last line is, that
 * is the step that killed the machine.
 *
 * Include before WifiPexStages.h (PexChip logs its sub-steps) and it is also
 * visible to WifiPrepare.h, which is included later in the same translation
 * unit.
 */
#ifndef WIFI_LOG_H
#define WIFI_LOG_H

STATIC EFI_FILE_PROTOCOL *mWifiLogFile = NULL;
STATIC BOOLEAN            mWifiLogTried = FALSE;

/* Not STATIC-only for readability: this file is included once. */
STATIC VOID
WifiLogOpen (
  VOID
  )
{
  EFI_HANDLE                        *Handles = NULL;
  UINTN                              Count = 0;
  UINTN                              Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *Fs;
  EFI_FILE_PROTOCOL                 *Root;
  EFI_FILE_PROTOCOL                 *File;

  if (mWifiLogTried) return;
  mWifiLogTried = TRUE;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                          NULL, &Count, &Handles))) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
                                        (VOID **)&Fs))) continue;
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) continue;
    if (!EFI_ERROR (Root->Open (Root, &File, L"\\SwitchWOA\\Wifi\\wifiprep.log",
                                EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
      mWifiLogFile = File;
      /* EFI_FILE_PROTOCOL::SetPosition with MAX_UINT64 seeks to end of file. */
      File->SetPosition (File, 0xFFFFFFFFFFFFFFFFULL);
      break;
    }
    Root->Close (Root);
  }
  gBS->FreePool (Handles);
}

STATIC VOID
WifiLogF (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Buffer[200];
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer) - 3, Format, Marker);
  VA_END (Marker);

  if (!mWifiLogTried) WifiLogOpen ();
  if (mWifiLogFile == NULL) return;

  if (Length > sizeof (Buffer) - 3) Length = sizeof (Buffer) - 3;
  Buffer[Length++] = '\r';
  Buffer[Length++] = '\n';
  mWifiLogFile->Write (mWifiLogFile, &Length, Buffer);
  mWifiLogFile->Flush (mWifiLogFile);
}



#endif
