// Driver.h - Common includes and driver-level entry points.
//
// Every .c file in this project includes this header first.

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>

#include "Public.h"
#include "Device.h"
#include "Queue.h"
#include "Actuator.h"
#include "Trace.h"

EXTERN_C_START

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AmtHapticEvtDeviceAdd;

EXTERN_C_END
