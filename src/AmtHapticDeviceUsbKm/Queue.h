// Queue.h - I/O queue entry points.

#pragma once

EXTERN_C_START

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtHapticQueueInitialize(
    _In_ WDFDEVICE Device
);

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AmtHapticEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP           AmtHapticEvtIoStop;

EXTERN_C_END
