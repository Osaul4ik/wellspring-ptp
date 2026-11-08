// Driver entry points and callbacks. Kernel-mode Driver Framework

#define INITGUID
#include <initguid.h>
#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, AmtHapticEvtDeviceAdd)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
// Initialize the driver and register the device callback.
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, AmtHapticEvtDeviceAdd);
    config.DriverPoolTag = AMT_HAPTIC_POOL_TAG;

    return WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);
}

NTSTATUS
AmtHapticEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
// Create and initialize the device on add.
{
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    return AmtHapticCreateDevice(DeviceInit);
}
