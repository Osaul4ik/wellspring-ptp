#pragma once

#include <hid/HidCommon.h>

// ============================================================================
// Per-model finger geometry.
//
// LOGICAL_MAXIMUM (X/Y) must equal this model's native raw coordinate range
// - i.e. exactly (x.max - x.min) / (y.max - y.min) from that model's entry
// in Bcm5974ConfigTable (Device.c). AmtInputParseFrame() (Input.c)
// clamps and offsets every reported contact into [0, x.max-x.min] /
// [0, y.max-y.min] for that same table entry, so this is the one thing that
// is NOT a measurement or an estimate - it MUST match Bcm5974ConfigTable
// exactly or the values below will silently disagree with what actually
// goes out on the wire, which is worse than the original bug.
//
// PHYSICAL_MAXIMUM (X/Y), in units of 0.1mm (UNIT_EXPONENT -2, UNIT SI cm),
// is the real physical size of the touch-sensitive surface. This is what
// Windows uses to convert reported finger movement into real-world
// millimeters before applying its own pointer-speed curve - if this lies
// about the surface being bigger than it really is, Windows overestimates
// how far the finger travelled and the cursor moves too fast/erratically
// for the actual motion made ("flying" cursor). This was the root cause on
// non-16" models: every model was shipping the MacBookPro16,1 values
// (157.8mm x 97.8mm) regardless of its actual, much smaller, trackpad.
//
// Only the MacBookPro16,1 (T2_16) row below is a real measurement, taken
// off a physical unit. Every other row is DERIVED, not measured: computed
// from that one confirmed measurement's raw-units-per-mm ratio applied to
// each model's own raw coordinate range. That is a reasonable estimate
// (T2_15's derived value lines up with the commonly cited ~130x79mm for
// that trackpad generation) but it is still an estimate, not a
// measurement, and sensor pitch is not guaranteed identical across
// hardware generations (T2 vs pre-T2 WELLSPRING8/9 in particular).
//
// To get an exact value for a given model instead of the derived one, boot
// macOS on that machine and read its own HID report descriptor, e.g.:
//   ioreg -c AppleMultitouchDevice -r -d 2 | grep -i "Physical"
// or dump it with `hidutil` / IORegistryExplorer and locate the Digitizer
// TLC's Physical Maximum items. Then just replace the PHYS_X10/PHYS_Y10
// pair for that model below - nothing else needs to change.
// ============================================================================

#define AAPL_WS_PTP_LOGX_FALLBACK 20000  // generic oversampled default, unchanged from before this table existed
#define AAPL_WS_PTP_LOGY_FALLBACK 12000
#define AAPL_WS_PTP_PHYSX10_FALLBACK 1578
#define AAPL_WS_PTP_PHYSY10_FALLBACK 978

#define AAPL_WS_PTP_LOGX_T2_16 15432  // MacBookPro16,1 (J152F) - calibrated from Live raw range -7456..7976
#define AAPL_WS_PTP_LOGY_T2_16 9446   // calibrated from Live raw range -163..9283
#define AAPL_WS_PTP_PHYSX10_T2_16 1578 // 157.8mm - measured
#define AAPL_WS_PTP_PHYSY10_T2_16 978  // 97.8mm - measured

#define AAPL_WS_PTP_LOGX_T2_15 15432  // MacBookPro15,1 (J680, 15" 2018)
#define AAPL_WS_PTP_LOGY_T2_15 9453
#define AAPL_WS_PTP_PHYSX10_T2_15 1293 // 129.3mm - derived, see header comment
#define AAPL_WS_PTP_PHYSY10_T2_15 786  // 78.6mm - derived

#define AAPL_WS_PTP_LOGX_T2_13 12992  // 13" T2 family: J140K/J132/J213/J214K/J223/J230K
#define AAPL_WS_PTP_LOGY_T2_13 7855
#define AAPL_WS_PTP_PHYSX10_T2_13 1089 // 108.9mm - derived
#define AAPL_WS_PTP_PHYSY10_T2_13 653  // 65.3mm - derived

#define AAPL_WS_PTP_LOGX_WS9 10173  // MacBookPro12,1 (2015 13" Force Touch, pre-T2)
#define AAPL_WS_PTP_LOGY_WS9 7006
#define AAPL_WS_PTP_PHYSX10_WS9 852  // 85.2mm - derived
#define AAPL_WS_PTP_PHYSY10_WS9 582  // 58.2mm - derived

#define AAPL_WS_PTP_LOGX_WS8 9760  // MacBookAir6,x/7,x (2013-2015, incl. the 2015 13" Air), pre-T2
#define AAPL_WS_PTP_LOGY_WS8 6750
#define AAPL_WS_PTP_PHYSX10_WS8 818  // 81.8mm - derived
#define AAPL_WS_PTP_PHYSY10_WS8 561  // 56.1mm - derived

// Finger collection, variant 1: resets Unit/UnitExponent/PhysicalMaximum to
// none/0/0 at the end (see the AUDIT FIX comment on
// AAPL_WELLSPRING_T2_FORCETOUCH_MOUSE_TLC below for why that reset exists -
// these are HID GLOBAL items and persist across collection boundaries).
// Used for fingers 1, 2 and 4 in AAPL_WELLSPRING_PTP_TLC.
#define AAPL_WELLSPRING_FINGER_COLLECTION_1(LOGXMAX, PHYSXMM10, LOGYMAX, PHYSYMM10) \
	BEGIN_COLLECTION, 0x02, /* Begin Collection: Logical */ \
		/* Begin a byte */ \
		LOGICAL_MAXIMUM, 0x01, /* Logical Maximum: 1 */ \
		USAGE, 0x47, /* Usage: Confidence */ \
		USAGE, 0x42, /* Usage: Tip switch */ \
		REPORT_COUNT, 0x02, /* Report Count: 2 */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		REPORT_COUNT, 0x06, /* Report Count: 6 */ \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		/* End of a byte */ \
		/* Begin of 4 bytes */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		REPORT_SIZE, 0x20, /* Report Size: 0x10 (2 bytes) */ \
		LOGICAL_MAXIMUM_3, 0xff, 0xff, 0xff, 0xff, /* Logical Maximum: 0xffffffff */ \
		USAGE, 0x51, /* Usage: Contract Identifier */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
		/* Begin of 4 bytes */ \
		USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
		LOGICAL_MAXIMUM_2, HID_U16_LO(LOGXMAX), HID_U16_HI(LOGXMAX), /* Logical Maximum: native raw X range for this model */ \
		REPORT_SIZE, 0x10, /* Report Size: 0x10 (2 bytes) */ \
		UNIT_EXPONENT, 0x0e, /* Unit exponent: -2 */ \
		UNIT, 0x11, /* Unit: SI Length (cm) */ \
		USAGE, 0x30, /* Usage: X */ \
		PHYSICAL_MAXIMUM_2, HID_U16_LO(PHYSXMM10), HID_U16_HI(PHYSXMM10), /* Physical Maximum: this model's real sensor width, in 0.1mm */ \
		REPORT_COUNT, 0x01, /* Report count: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM_2, HID_U16_LO(PHYSYMM10), HID_U16_HI(PHYSYMM10), /* Physical Maximum: this model's real sensor height, in 0.1mm */ \
		LOGICAL_MAXIMUM_2, HID_U16_LO(LOGYMAX), HID_U16_HI(LOGYMAX), /* Logical Maximum: native raw Y range for this model */ \
		USAGE, 0x31, /* Usage: Y */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM, 0x00, /* Physical Maximum: 0 */ \
		UNIT_EXPONENT, 0x00, /* Unit exponent: 0 */ \
		UNIT, 0x00, /* Unit: None */ \
		/* End of 4 bytes */ \
	END_COLLECTION /* End Collection */ \

// Finger collection, variant 2: identical geometry fields, no trailing
// Unit/PhysicalMaximum reset (see variant 1's comment). Used for fingers 3
// and 5 in AAPL_WELLSPRING_PTP_TLC.
#define AAPL_WELLSPRING_FINGER_COLLECTION_2(LOGXMAX, PHYSXMM10, LOGYMAX, PHYSYMM10) \
	BEGIN_COLLECTION, 0x02, /* Begin Collection: Logical */ \
		/* Begin a byte */ \
		LOGICAL_MAXIMUM, 0x01, /* Logical Maximum: 1 */ \
		USAGE, 0x47, /* Usage: Confidence */ \
		USAGE, 0x42, /* Usage: Tip switch */ \
		REPORT_COUNT, 0x02, /* Report Count: 2 */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		REPORT_COUNT, 0x06, /* Report Count: 6 */ \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		/* End of a byte */ \
		/* Begin of 4 bytes */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		REPORT_SIZE, 0x20, /* Report Size: 0x10 (2 bytes) */ \
		LOGICAL_MAXIMUM_3, 0xff, 0xff, 0xff, 0xff, /* Logical Maximum: 0xffffffff */ \
		USAGE, 0x51, /* Usage: Contract Identifier */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
		/* Begin of 4 bytes */ \
		USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
		LOGICAL_MAXIMUM_2, HID_U16_LO(LOGXMAX), HID_U16_HI(LOGXMAX), /* Logical Maximum: native raw X range for this model */ \
		REPORT_SIZE, 0x10, /* Report Size: 0x10 (2 bytes) */ \
		UNIT_EXPONENT, 0x0e, /* Unit exponent: -2 */ \
		UNIT, 0x11, /* Unit: SI Length (cm) */ \
		USAGE, 0x30, /* Usage: X */ \
		PHYSICAL_MAXIMUM_2, HID_U16_LO(PHYSXMM10), HID_U16_HI(PHYSXMM10), /* Physical Maximum: this model's real sensor width, in 0.1mm */ \
		REPORT_COUNT, 0x01, /* Report count: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM_2, HID_U16_LO(PHYSYMM10), HID_U16_HI(PHYSYMM10), /* Physical Maximum: this model's real sensor height, in 0.1mm */ \
		LOGICAL_MAXIMUM_2, HID_U16_LO(LOGYMAX), HID_U16_HI(LOGYMAX), /* Logical Maximum: native raw Y range for this model */ \
		USAGE, 0x31, /* Usage: Y */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
	END_COLLECTION /* End Collection */ \

// Separate mouse collection for synthetic right-click delivery. Always
// relative (0,0) - no coordinate geometry, so no per-model parameters.
#define AAPL_WELLSPRING_T2_FORCETOUCH_MOUSE_TLC \
	USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
	USAGE, 0x02, /* Usage: Mouse */ \
	BEGIN_COLLECTION, 0x01, /* Begin Collection: Application */ \
		REPORT_ID, REPORTID_STANDARDMOUSE, \
		USAGE, 0x01, /* Usage: Pointer */ \
		BEGIN_COLLECTION, 0x00, /* Begin Collection: Physical */ \
			USAGE_PAGE, 0x09, /* Usage Page: Button */ \
			USAGE_MINIMUM, 0x01, /* Button 1 */ \
			USAGE_MAXIMUM, 0x03, /* Button 3 */ \
			LOGICAL_MINIMUM, 0x00, \
			LOGICAL_MAXIMUM, 0x01, \
			REPORT_COUNT, 0x03, \
			REPORT_SIZE, 0x01, \
			INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
			REPORT_COUNT, 0x01, \
			REPORT_SIZE, 0x05, \
			INPUT, 0x03, /* Input: (Const, Var, Abs) - padding */ \
			USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
			USAGE, 0x30, /* Usage: X */ \
			USAGE, 0x31, /* Usage: Y */ \
			LOGICAL_MINIMUM, 0x81, /* -127 */ \
			LOGICAL_MAXIMUM, 0x7f, /* 127 */ \
			/* AUDIT FIX (cursor teleport to bottom-right on force */ \
			/* touch): Unit/Unit Exponent/Physical Min/Max are HID */ \
			/* GLOBAL items - they persist across top-level */ \
			/* collection boundaries, not just within one. The PTP */ \
			/* TLC above leaves Unit = Time(Second), Unit Exponent */ \
			/* = -4, and Physical Maximum = 0xffffffff in effect */ \
			/* from its Scan Time field and never resets them before */ \
			/* this (later) collection. Without an explicit reset */ \
			/* here, X/Y silently inherited that huge bogus physical */ \
			/* range, which is what drove the cursor to the extreme */ \
			/* (bottom-right) logical position. Same reset already */ \
			/* used at the end of AAPL_WELLSPRING_FINGER_ */ \
			/* COLLECTION_1 above, for the identical reason. */ \
			PHYSICAL_MINIMUM, 0x00, \
			PHYSICAL_MAXIMUM, 0x00, \
			UNIT_EXPONENT, 0x00, \
			UNIT, 0x00, \
			REPORT_SIZE, 0x08, \
			REPORT_COUNT, 0x02, \
			INPUT, 0x06, /* Input: (Data, Var, Rel) - always sent as 0 */ \
		END_COLLECTION, /* End Collection */ \
	END_COLLECTION /* End Collection */

// Full PTP top-level collection, parameterized by this model's logical/
// physical X/Y geometry. Every finger slot gets the SAME geometry (all 5
// fingers touch the same physical surface) - only the reset behavior
// differs between variant 1 (fingers 1/2/4) and variant 2 (fingers 3/5),
// inherited unchanged from the original single-model layout.
#define AAPL_WELLSPRING_PTP_TLC(LOGXMAX, PHYSXMM10, LOGYMAX, PHYSYMM10) \
	USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
	USAGE, 0x05, /* Usage: Touch Pad */ \
	BEGIN_COLLECTION, 0x01, /* Begin Collection: Application */ \
		REPORT_ID, REPORTID_MULTITOUCH, /* Report ID: Multi-touch */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_FINGER_COLLECTION_1(LOGXMAX, PHYSXMM10, LOGYMAX, PHYSYMM10), /* 1 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_FINGER_COLLECTION_1(LOGXMAX, PHYSXMM10, LOGYMAX, PHYSYMM10), /* 2 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_FINGER_COLLECTION_2(LOGXMAX, PHYSXMM10, LOGYMAX, PHYSYMM10), /* 3 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_FINGER_COLLECTION_1(LOGXMAX, PHYSXMM10, LOGYMAX, PHYSYMM10), /* 4 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_FINGER_COLLECTION_2(LOGXMAX, PHYSXMM10, LOGYMAX, PHYSYMM10), /* 5 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		UNIT_EXPONENT, 0x0c, /* Unit exponent: -4 */ \
		UNIT_2, 0x01, 0x10, /* Time: Second */ \
		PHYSICAL_MAXIMUM_3, 0xff, 0xff, 0x00, 0x00, \
		LOGICAL_MAXIMUM_3, 0xff, 0xff, 0x00, 0x00, \
		USAGE, 0x56, /* Usage: Scan Time */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		USAGE, 0x54, /* Usage: Contact Count */ \
		LOGICAL_MAXIMUM, 0x7f, \
		REPORT_SIZE, 0x08, \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		USAGE_PAGE, 0x09, /* Usage Page: Button */ \
		USAGE, 0x01, /* Button 1 */ \
		LOGICAL_MAXIMUM, 0x01, \
		REPORT_SIZE, 0x01, \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_COUNT, 0x07, \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		REPORT_ID, REPORTID_DEVICE_CAPS, \
		USAGE, 0x55, /* Usage: Maximum Contacts */ \
		USAGE, 0x59, /* Usage: Touchpad Button Type*/ \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM_2, 0xff, 0x00, \
		REPORT_SIZE, 0x08, \
		REPORT_COUNT, 0x02, \
		FEATURE, 0x02, \
		USAGE_PAGE_1, 0x00, 0xff, \
		REPORT_ID, REPORTID_PTPHQA, \
		USAGE, 0xc5, \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM_2, 0xff, 0x00, \
		REPORT_SIZE, 0x08, \
		REPORT_COUNT_2, 0x00, 0x01, \
		FEATURE, 0x02, \
	END_COLLECTION /* End Collection */

// Back-compat: the original fixed-geometry macro name, now just the
// MacBookPro16,1 instantiation of the parameterized TLC above. Nothing
// outside this header should need this anymore (Hid.c builds every model's
// descriptor from AAPL_WELLSPRING_PTP_TLC directly), kept only in case
// something still references it.
#define AAPL_WELLSPRING_T2_PTP_TLC \
	AAPL_WELLSPRING_PTP_TLC(AAPL_WS_PTP_LOGX_T2_16, AAPL_WS_PTP_PHYSX10_T2_16, \
	                        AAPL_WS_PTP_LOGY_T2_16, AAPL_WS_PTP_PHYSY10_T2_16)