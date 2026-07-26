/*++
  Shared IOCTL / report layout for QstVHid (kernel + user mode).
--*/

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <initguid.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

// {A7C3E9F1-4B2D-4E8A-9C1F-6D5E8A7B3C2D}
#ifdef _KERNEL_MODE
DEFINE_GUID(GUID_DEVINTERFACE_QSTVHID,
    0xa7c3e9f1, 0x4b2d, 0x4e8a, 0x9c, 0x1f, 0x6d, 0x5e, 0x8a, 0x7b, 0x3c, 0x2d);
#else
// Usermode: declare only; define once in virtual_hid.cpp
EXTERN_C const GUID GUID_DEVINTERFACE_QSTVHID;
#endif

#define QSTVHID_DEVICE_TYPE 0x8000

#define IOCTL_QSTVHID_SUBMIT_REPORT \
    CTL_CODE(QSTVHID_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define QSTVHID_REPORT_ID_KEYBOARD   0x01
#define QSTVHID_REPORT_ID_MOUSE_REL  0x02
#define QSTVHID_REPORT_ID_MOUSE_ABS  0x03

#define QSTVHID_REPORT_LEN_KEYBOARD  9
#define QSTVHID_REPORT_LEN_MOUSE_REL 8
#define QSTVHID_REPORT_LEN_MOUSE_ABS 6

#define QSTVHID_MAX_REPORT_PAYLOAD   64

#pragma pack(push, 1)
typedef struct _QSTVHID_SUBMIT_REPORT {
    UCHAR ReportId;
    UCHAR Length;           /* bytes in Data[], Data[0] == ReportId */
    UCHAR Data[QSTVHID_MAX_REPORT_PAYLOAD];
} QSTVHID_SUBMIT_REPORT, *PQSTVHID_SUBMIT_REPORT;
#pragma pack(pop)
