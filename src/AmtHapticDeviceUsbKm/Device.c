// Device.c - Device creation and hardware preparation.

#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtHapticCreateDevice)
#pragma alloc_text (PAGE, AmtHapticEvtDevicePrepareHardware)
#endif

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtHapticCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
// Create the device object, its context, its click transfer, its device
// interface and its queue.
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES        attributes;
    WDFDEVICE                    device;
    PDEVICE_CONTEXT              context;
    NTSTATUS                     status;

    // Opens of the device interface are access-checked against these, not
    // against whatever the PDO's INF or class says: administrators and SYSTEM
    // full access, other signed-in users read/write only. That is what lets an
    // unelevated control panel fire a click, and no more than that - the only
    // thing this device can do is play a fixed haptic waveform.
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)");

    PAGED_CODE();

    // This driver is the function driver for its interface (a filter would not
    // own the device). It is therefore the power policy owner by default, and
    // deliberately configures NO idle power management: the first click after
    // an idle suspend would pay the resume latency, which is exactly what a
    // haptic response to a press must not do. System sleep is handled by the
    // framework as usual.
    //
    // No WdfDeviceInitSetDeviceType: an FDO takes the device type of the device
    // below it, and overriding that is for PDOs and control devices.
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, TRUE);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = AmtHapticEvtDevicePrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // WDF zero-fills new context memory: UsbDevice, counters and flags start
    // cleared.
    context = DeviceGetContext(device);

    AmtHapticTraceLoadDebugModeFromRegistry(device, context);

    status = AmtHapticAllocateTransfer(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_AMT_HAPTIC, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return AmtHapticQueueInitialize(device);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtHapticEvtDevicePrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
// Create the USB target, check that this really is the actuator interface of a
// supported trackpad, and select its configuration. Called on every start, so
// the USB device object is created only once.
{
    PDEVICE_CONTEXT                    context = DeviceGetContext(Device);
    WDF_USB_DEVICE_CREATE_CONFIG       createConfig;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS selectParams;
    USB_DEVICE_DESCRIPTOR              deviceDescriptor;
    USB_INTERFACE_DESCRIPTOR           interfaceDescriptor;
    WDFUSBINTERFACE                    usbInterface;
    NTSTATUS                           status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    if (context->UsbDevice == NULL) {
        WDF_USB_DEVICE_CREATE_CONFIG_INIT(&createConfig, USBD_CLIENT_CONTRACT_VERSION_602);

        status = WdfUsbTargetDeviceCreateWithParameters(
            Device,
            &createConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &context->UsbDevice);

        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    // The INF binds by hardware ID, but the driver does not rely on that: it
    // sends nothing to a device it has not positively identified, because the
    // report it sends is meaningful to this actuator only.
    WdfUsbTargetDeviceGetDeviceDescriptor(context->UsbDevice, &deviceDescriptor);

    if (!AmtHapticIsSupportedDevice(deviceDescriptor.idVendor, deviceDescriptor.idProduct)) {
        AmtHapticTrace(context, "unsupported device %04X:%04X\n",
            deviceDescriptor.idVendor, deviceDescriptor.idProduct);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    // Bound to ONE interface of the composite device, never to the composite
    // parent and never to the touchpad's own interface.
    if (WdfUsbTargetDeviceGetNumInterfaces(context->UsbDevice) != 1) {
        AmtHapticTrace(context, "expected a single-interface function device\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    usbInterface = WdfUsbTargetDeviceGetInterface(context->UsbDevice, 0);

    if (WdfUsbInterfaceGetInterfaceNumber(usbInterface) != AMT_HAPTIC_INTERFACE_NUMBER) {
        AmtHapticTrace(context, "wrong interface number %u\n",
            (ULONG)WdfUsbInterfaceGetInterfaceNumber(usbInterface));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    WdfUsbInterfaceGetDescriptor(usbInterface, 0, &interfaceDescriptor);

    if (interfaceDescriptor.bInterfaceClass != USB_DEVICE_CLASS_HUMAN_INTERFACE) {
        AmtHapticTrace(context, "interface is not HID (class 0x%02X)\n",
            interfaceDescriptor.bInterfaceClass);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&selectParams);

    status = WdfUsbTargetDeviceSelectConfig(
        context->UsbDevice,
        WDF_NO_OBJECT_ATTRIBUTES,
        &selectParams);

    if (!NT_SUCCESS(status)) {
        AmtHapticTrace(context, "SelectConfig failed, status=0x%08X\n", status);
        return status;
    }

    context->UsbInterface    = selectParams.Types.SingleInterface.ConfiguredUsbInterface;
    context->InterfaceNumber = WdfUsbInterfaceGetInterfaceNumber(context->UsbInterface);

    AmtHapticTrace(context, "ready: %04X:%04X interface %u\n",
        deviceDescriptor.idVendor, deviceDescriptor.idProduct, (ULONG)context->InterfaceNumber);

    return STATUS_SUCCESS;
}
