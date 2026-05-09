#include <ntddk.h>

#define DEVICE_NAME L"\\Device\\SimpleEcho"
#define SYMBOLIC_NAME L"\\DosDevices\\SimpleEcho"

#define IOCTL_SIMPLE_ECHO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

static void SimpleEchoUnload(_In_ PDRIVER_OBJECT DriverObject);
static NTSTATUS SimpleEchoCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
static NTSTATUS SimpleEchoDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicName;

    RtlInitUnicodeString(&deviceName, DEVICE_NAME);
    RtlInitUnicodeString(&symbolicName, SYMBOLIC_NAME);

    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &deviceObject);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = IoCreateSymbolicLink(&symbolicName, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }

    DriverObject->DriverUnload = SimpleEchoUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = SimpleEchoCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = SimpleEchoCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SimpleEchoDeviceControl;

    KdPrint(("SimpleEcho: loaded\n"));
    return STATUS_SUCCESS;
}

static void SimpleEchoUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symbolicName;

    RtlInitUnicodeString(&symbolicName, SYMBOLIC_NAME);
    IoDeleteSymbolicLink(&symbolicName);

    if (DriverObject->DeviceObject != NULL) {
        IoDeleteDevice(DriverObject->DeviceObject);
    }

    KdPrint(("SimpleEcho: unloaded\n"));
}

static NTSTATUS CompleteRequest(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS SimpleEchoCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return CompleteRequest(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS SimpleEchoDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;

    if (controlCode != IOCTL_SIMPLE_ECHO) {
        return CompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }

    if (buffer == NULL || inputLength == 0) {
        return CompleteRequest(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    if (outputLength < inputLength) {
        return CompleteRequest(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }

    /*
     * METHOD_BUFFERED uses one system buffer for input and output. The "echo"
     * behavior is therefore just reporting the same bytes back to the caller.
     */
    KdPrint(("SimpleEcho: echoing %lu bytes\n", inputLength));
    return CompleteRequest(Irp, STATUS_SUCCESS, inputLength);
}
