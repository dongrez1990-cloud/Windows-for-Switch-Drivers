/*
 * Protocol constants for Nintendo Switch Joy-Con over the console rail UART.
 * Ported from switchroot l4t-kernel drivers/input/joystick/joycon-serdev.c
 * (Daniel Ogorchock / CTCaer).  See ../ref/PROTOCOL.md for the frame table.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef SWITCHJOYCON_JC_H
#define SWITCHJOYCON_JC_H

/* UART register indexes (byte offset / 4, Tegra HSUART is 16550-compatible). */
enum {
    JcData = 0x00 / 4, JcIer = 0x04 / 4, JcIir = 0x08 / 4, JcFcr = 0x08 / 4,
    JcLcr = 0x0C / 4, JcMcr = 0x10 / 4, JcLsr = 0x14 / 4, JcMsr = 0x18 / 4,
    JcScratch = 0x1C / 4, JcIrda = 0x20 / 4
};

/* UART offset 0x20: line inversion (Linux TEGRA_UART_IRDA_CSR << 2). */
enum { JcInvertRxd = 0x01, JcInvertTxd = 0x02, JcInvertCts = 0x04, JcInvertRts = 0x08 };

enum {
    JcLsrDataReady = 0x01, JcLsrOverrun = 0x02, JcLsrErrors = 0x1E,
    JcLsrTxEmpty = 0x20, JcLsrTxDone = 0x40,
    JcIerRx = 0x15, JcIerTx = 0x02,
    JcMcrRts = 0x02, JcMcrFlow = 0x62
};

/* UART headers. */
enum {
    JcCmdExtSend = 0x91, JcCmdExtRet = 0x92, JcCmdInitRet = 0x94,
    JcCmdHandshake = 0xA5, JcCmdHoriReport = 0x9A,
    JcTxMagic0 = 0x19, JcTxMagic1 = 0x01, JcTxMagic2 = 0x03,
    JcRxMagic0 = 0x19, JcRxMagic1 = 0x81, JcRxMagic2 = 0x03,
    JcHeaderSize = 12, JcMaxData = 64, JcMaxPacket = JcHeaderSize + JcMaxData
};

/* Handshake steps carried in header_data[0]. */
enum {
    JcInitMac = 0x01, JcInitHidConnect = 0x10, JcInitHidDisconnect = 0x11,
    JcInitSetHidRate = 0x12, JcInitSetBaudrate = 0x20
};

/* Input report ids. */
enum {
    JcInputAck = 0x1F, JcInputButtonEvent = 0x3F, JcInputSubcmdReply = 0x21,
    JcInputImuData = 0x30, JcInputMcuData = 0x31, JcInputUsbResponse = 0x81,
    JcOutputRumbleSubcmd = 0x01
};

/* Subcommand ids. */
enum {
    JcSubcmdState = 0x00, JcSubcmdReqDevInfo = 0x02, JcSubcmdSetReportMode = 0x03,
    JcSubcmdSpiRead = 0x10, JcSubcmdRequestReport = 0x1F, JcSubcmdEnableImu = 0x40,
    JcSubcmdSetPlayerLights = 0x30
};

/* 24-bit button map, little endian in button_status[3]. */
enum {
    JcBtnY = 1u << 0, JcBtnX = 1u << 1, JcBtnB = 1u << 2, JcBtnA = 1u << 3,
    JcBtnSrR = 1u << 4, JcBtnSlR = 1u << 5, JcBtnR = 1u << 6, JcBtnZr = 1u << 7,
    JcBtnMinus = 1u << 8, JcBtnPlus = 1u << 9, JcBtnRStick = 1u << 10,
    JcBtnLStick = 1u << 11, JcBtnHome = 1u << 12, JcBtnCapture = 1u << 13,
    JcBtnDown = 1u << 16, JcBtnUp = 1u << 17, JcBtnRight = 1u << 18,
    JcBtnLeft = 1u << 19, JcBtnSrL = 1u << 20, JcBtnSlL = 1u << 21,
    JcBtnL = 1u << 22, JcBtnZl = 1u << 23
};

/* Default calibration when the sticks have not been read over SPI yet. */
enum { JcCalCentre = 2048, JcCalMax = 3072, JcCalMin = 1024 };

/* 15 ms is what the Joy-Con is told to report at (SET_HID_RATE 0x0F). */
enum { JcReportPeriodMs = 15, JcDetachTimeoutMs = 500 };

typedef struct {
    UCHAR Uart;                 /* frame byte 0 */
    UCHAR Header[JcHeaderSize];
    UCHAR Data[JcMaxData];
    ULONG DataLength;
} JC_PACKET;

typedef struct {
    ULONG Buttons;              /* JcBtn* bits */
    SHORT Axis[4];              /* X, Y, Z, Rz in -32767..32767 */
    UCHAR Hat;                  /* 0..7, 8 = released */
} JC_PAD;

static __inline UCHAR JcCrc8(const UCHAR *Bytes, ULONG Count)
{
    UCHAR Crc = 0;
    for (ULONG i = 0; i < Count; ++i) {
        Crc ^= Bytes[i];
        for (ULONG bit = 0; bit < 8; ++bit)
            Crc = (UCHAR)((Crc << 1) ^ ((Crc & 0x80) ? 0x8D : 0));
    }
    return Crc;
}

/* Build a 19 01 03 frame; caller passes header_data[5] and data payload. */
static __inline void JcBuildPacket(JC_PACKET *P, UCHAR Command, const UCHAR Header[5],
                                   const UCHAR *Data, ULONG DataLength)
{
    P->Header[0] = JcTxMagic0;
    P->Header[1] = JcTxMagic1;
    P->Header[2] = JcTxMagic2;
    P->Header[3] = (UCHAR)(7 + DataLength);
    P->Header[4] = 0;                 /* pad */
    P->Header[5] = Command;
    for (ULONG i = 0; i < 5; ++i) P->Header[6 + i] = Header ? Header[i] : 0;
    P->Header[11] = (Command == JcCmdHandshake) ? 0 : JcCrc8(&P->Header[4], 7);
    if (Data && DataLength) for (ULONG i = 0; i < DataLength; ++i) P->Data[i] = Data[i];
    P->DataLength = DataLength;
}

/* Map a 12-bit stick value: the 0.2.9 formula, unchanged, plus one addition --
 * inside +-150 raw counts of the centre the output is exactly 0.  That is the whole
 * dead zone fix: the stick idles within ~10-14 counts, but a re-attach can leave the
 * measured centre a few dozen counts off, and 150 covers both without eating travel. */
#define JC_STICK_DEAD_RAW 200

static __inline SHORT JcMapStick(int Raw, int Centre, int Min, int Max)
{
    int Delta = Raw - Centre;
    long Span;
    long Scaled;

    if (Delta <= JC_STICK_DEAD_RAW && Delta >= -JC_STICK_DEAD_RAW) return 0;

    Span = Delta >= 0 ? (Max - Centre) : (Centre - Min);
    if (Span <= 0) Span = 1024;
    Scaled = (long)Delta * 32767 / Span;
    if (Scaled > 32767) Scaled = 32767;
    if (Scaled < -32767) Scaled = -32767;
    return (SHORT)Scaled;
}

/* 24-bit little-endian field extract (Linux field_extract for <=24 bits). */
static __inline ULONG JcField24(const UCHAR *Bytes)
{
    return (ULONG)Bytes[0] | ((ULONG)Bytes[1] << 8) | ((ULONG)Bytes[2] << 16);
}

#endif

/* ---- desktop mapping (same idea as switchsio / switchroot) ---- */
typedef struct { UCHAR Id; UCHAR Buttons; UCHAR Dx; UCHAR Dy; UCHAR Wheel; UCHAR Pan; } JC_MOUSE_REPORT;
typedef struct { UCHAR Id; UCHAR Modifiers; UCHAR Reserved; UCHAR Keys[6]; } JC_KEYBOARD_REPORT;

/* Stick -> relative pointer motion: the 0.2.9 formula, unchanged (the user reports
 * that it behaved fine there; every rewrite I tried only made it worse). */
static __inline UCHAR JcPointerAxis(int Axis, ULONG Ms, long *Fraction)
{
    const int DeadZone = 6000;
    const int MaxSpeed = 800;
    int Magnitude = Axis < 0 ? -Axis : Axis;
    int Speed;
    long Total;
    if (Magnitude <= DeadZone) { *Fraction = 0; return 0; }
    Speed = (Magnitude - DeadZone) * MaxSpeed / (32767 - DeadZone);
    if (Axis < 0) Speed = -Speed;
    Total = *Fraction + (long)Speed * (long)Ms;
    *Fraction = Total % 1000;
    return (UCHAR)(Total / 1000);
}