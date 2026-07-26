/* QstVHid - KMDF + VHF virtual HID source driver */

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>
#include <initguid.h>

#include "qst_vhid_ioctl.h"
#include "hid_report_desc.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD QstVhidEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP QstVhidEvtDeviceCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL QstVhidEvtIoDeviceControl;

typedef struct _DEVICE_CONTEXT {
    VHFHANDLE VhfHandle;
    WDFQUEUE DefaultQueue;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, QstVhidEvtDeviceAdd)
#pragma alloc_text(PAGE, QstVhidEvtDeviceCleanup)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, QstVhidEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    return status;
}

NTSTATUS
QstVhidEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    PDEVICE_CONTEXT ctx;
    VHF_CONFIG vhfConfig;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    WDF_OBJECT_ATTRIBUTES queueAttributes;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = QstVhidEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = DeviceGetContext(device);
    ctx->VhfHandle = NULL;
    ctx->DefaultQueue = NULL;

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_QSTVHID, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = QstVhidEvtIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.SynchronizationScope = WdfSynchronizationScopeQueue;

    status = WdfIoQueueCreate(device, &queueConfig, &queueAttributes, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx->DefaultQueue = queue;

    VHF_CONFIG_INIT(&vhfConfig,
        WdfDeviceWdmGetDeviceObject(device),
        (USHORT)QSTVHID_REPORT_DESCRIPTOR_SIZE,
        (PUCHAR)g_QstVhidReportDescriptor);

    vhfConfig.VendorID = 0x5153;
    vhfConfig.ProductID = 0x5648;
    vhfConfig.VersionNumber = 0x0100;
    vhfConfig.VhfClientContext = ctx;

    status = VhfCreate(&vhfConfig, &ctx->VhfHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = VhfStart(ctx->VhfHandle);
    if (!NT_SUCCESS(status)) {
        VhfDelete(ctx->VhfHandle, TRUE);
        ctx->VhfHandle = NULL;
        return status;
    }

    return STATUS_SUCCESS;
}

VOID
QstVhidEvtDeviceCleanup(
    _In_ WDFOBJECT DeviceObject
    )
{
    PDEVICE_CONTEXT ctx;
    PAGED_CODE();

    ctx = DeviceGetContext(DeviceObject);
    if (ctx->VhfHandle != NULL) {
        VhfDelete(ctx->VhfHandle, TRUE);
        ctx->VhfHandle = NULL;
    }
}

VOID
QstVhidEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    NTSTATUS status;
    PDEVICE_CONTEXT ctx;
    WDFDEVICE device;
    PQSTVHID_SUBMIT_REPORT submit = NULL;
    size_t bufLen = 0;
    HID_XFER_PACKET packet;
    UCHAR localReport[QSTVHID_MAX_REPORT_PAYLOAD];

    UNREFERENCED_PARAMETER(OutputBufferLength);

    device = WdfIoQueueGetDevice(Queue);
    ctx = DeviceGetContext(device);

    if (IoControlCode != IOCTL_QSTVHID_SUBMIT_REPORT) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (ctx->VhfHandle == NULL) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    if (InputBufferLength < FIELD_OFFSET(QSTVHID_SUBMIT_REPORT, Data)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(QSTVHID_SUBMIT_REPORT),
        (PVOID*)&submit, &bufLen);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    if (submit->Length == 0 || submit->Length > QSTVHID_MAX_REPORT_PAYLOAD) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if (submit->Data[0] != submit->ReportId) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    RtlCopyMemory(localReport, submit->Data, submit->Length);

    RtlZeroMemory(&packet, sizeof(packet));
    packet.reportBuffer = localReport;
    packet.reportBufferLen = submit->Length;
    packet.reportId = submit->ReportId;

    status = VhfReadReportSubmit(ctx->VhfHandle, &packet);
    WdfRequestComplete(Request, status);
}
