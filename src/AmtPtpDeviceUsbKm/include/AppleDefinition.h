#pragma once

#define USB_VENDOR_ID_APPLE		0x05ac

/*
 * Apple T2 USB trackpads.
 *
 * Names and PIDs match the Linux kernel's T2 device IDs (see
 * drivers/hid/hid-ids.h and the "T2-Attached Devices" block in
 * drivers/input/mouse/bcm5974.c, as carried by the aunali1/linux-mbp-arch
 * out-of-tree patch series 4002-4009 "HID: apple: Add support for
 * <model> keyboard/trackpad"). Kept as USB_DEVICE_ID_APPLE_T2_xxx here
 * instead of the upstream WELLSPRINGT2_xxx names to match this driver's
 * existing naming convention; the codename in each comment is the
 * upstream/Apple internal board ID (e.g. J132) for cross-reference.
 */
#define USB_DEVICE_ID_APPLE_T2_J140K  0x027a  /* MacBookAir8,1  (2018) */
#define USB_DEVICE_ID_APPLE_T2_J132   0x027b  /* MacBookPro15,2 (2018, 13", 4x TB3) */
#define USB_DEVICE_ID_APPLE_T2_J680   0x027c  /* MacBookPro15,1 (2018, 15") */
#define USB_DEVICE_ID_APPLE_T2_J213   0x027d  /* MacBookPro15,4 (2019, 13", 2x TB3) */
#define USB_DEVICE_ID_APPLE_T2_J214K  0x027e  /* MacBookPro16,2 (2020, 13", 4x TB3) */
#define USB_DEVICE_ID_APPLE_T2_J223   0x027f  /* MacBookPro16,3 (2020, 13", 2x TB3) */
#define USB_DEVICE_ID_APPLE_T2_J230K  0x0280  /* MacBookAir9,1  (2020) */

// Apple T2 USB trackpad — MacBookPro16,1 (16-inch, 2019), codename J152F.
// Confirmed on a real device; previously fell through to the fallback entry.
#define USB_DEVICE_ID_APPLE_T2_J152F  0x0340

/* Back-compat aliases for the old (pre-rename) constant names. */
#define USB_DEVICE_ID_APPLE_T2_7A  USB_DEVICE_ID_APPLE_T2_J140K
#define USB_DEVICE_ID_APPLE_T2_7B  USB_DEVICE_ID_APPLE_T2_J132
#define USB_DEVICE_ID_APPLE_T2_7C  USB_DEVICE_ID_APPLE_T2_J680
#define USB_DEVICE_ID_APPLE_T2_7D  USB_DEVICE_ID_APPLE_T2_J213
#define USB_DEVICE_ID_APPLE_T2_16  USB_DEVICE_ID_APPLE_T2_J152F

/*
 * Apple WELLSPRING9 USB trackpad - MacBookPro12,1 (2015, 13" Retina,
 * Force Touch, no Touch Bar). Not T2. TYPE4 packet format - this is
 * literally where TYPE4 (pressure field) was introduced upstream, per
 * "Input: bcm5974 - Add support for the 2015 Macbook Pro" (commit
 * d58069265c9d, Henrik Rydberg / John Horan, kernel 4.2).
 */
#define USB_DEVICE_ID_APPLE_WELLSPRING9_ANSI  0x0272
#define USB_DEVICE_ID_APPLE_WELLSPRING9_ISO   0x0273
#define USB_DEVICE_ID_APPLE_WELLSPRING9_JIS   0x0274

/*
 * Apple WELLSPRING8 USB trackpad - MacBookAir6,1 / MacBookAir6,2 (2013)
 * through MacBookAir7,1 / MacBookAir7,2 (2015). Not a T2 device (no T2
 * chip until 2018) - same board/trackpad kept unchanged across all of
 * these model years, so one PID pair covers all of them. TYPE3 packet
 * format, per drivers/input/mouse/bcm5974.c.
 */
#define USB_DEVICE_ID_APPLE_WELLSPRING8_ANSI  0x0290
#define USB_DEVICE_ID_APPLE_WELLSPRING8_ISO   0x0291

#define USB_DEVICE_ID_DEFAULT_FALLBACK 0xffff

/* button data structure */
struct TRACKPAD_BUTTON_DATA {
	UCHAR unknown1;		/* constant */
	UCHAR button;			/* left button */
	UCHAR rel_x;			/* relative x coordinate */
	UCHAR rel_y;			/* relative y coordinate */
};

/* trackpad header types */
enum TRACKPAD_TYPE {
	TYPE1,			/* plain trackpad */
	TYPE2,			/* button integrated in trackpad */
	TYPE3,			/* additional header fields since June 2013 */
	TYPE4,			/* additional header field for pressure data */
	TYPE5			/* format for magic trackpad 2 */
};

/* Trackpad finger data offsets, le16-aligned */
#define HEADER_TYPE1		(13 * sizeof(USHORT))
#define HEADER_TYPE2		(15 * sizeof(USHORT))
#define HEADER_TYPE3		(19 * sizeof(USHORT))
#define HEADER_TYPE4		(23 * sizeof(USHORT))
#define HEADER_TYPE5		( 6 * sizeof(USHORT))

/* Trackpad button data offsets */
#define BUTTON_TYPE1		0
#define BUTTON_TYPE2		15
#define BUTTON_TYPE3		23
#define BUTTON_TYPE4		31
#define BUTTON_TYPE5		1

/* List of device capability bits */
#define HAS_INTEGRATED_BUTTON	1

/* Trackpad finger data block size */
#define FSIZE_TYPE1		(14 * sizeof(USHORT))
#define FSIZE_TYPE2		(14 * sizeof(USHORT))
#define FSIZE_TYPE3		(14 * sizeof(USHORT))
#define FSIZE_TYPE4		(15 * sizeof(USHORT))
#define FSIZE_TYPE5		(9)

/* Offset from header to finger struct */
#define DELTA_TYPE1		(0 * sizeof(USHORT))
#define DELTA_TYPE2		(0 * sizeof(USHORT))
#define DELTA_TYPE3		(0 * sizeof(USHORT))
#define DELTA_TYPE4		(1 * sizeof(USHORT))
#define DELTA_TYPE5		(0 * sizeof(USHORT))

/* USB control message mode switch data */
#define USBMSG_TYPE1	8, 0x300, 0, 0, 0x1, 0x8
#define USBMSG_TYPE2	8, 0x300, 0, 0, 0x1, 0x8
#define USBMSG_TYPE3	8, 0x300, 0, 0, 0x1, 0x8
#define USBMSG_TYPE4	2, 0x302, 2, 1, 0x1, 0x0
#define USBMSG_TYPE5	2, 0x302, 1, 1, 0x1, 0x0

// Keep the mode-switch table size and index consistent.
C_ASSERT(0 < 8);  // USBMSG_TYPE1: um_switch_idx=0 < um_size=8
C_ASSERT(0 < 8);  // USBMSG_TYPE2: um_switch_idx=0 < um_size=8
C_ASSERT(0 < 8);  // USBMSG_TYPE3: um_switch_idx=0 < um_size=8
C_ASSERT(1 < 2);  // USBMSG_TYPE4: um_switch_idx=1 < um_size=2
C_ASSERT(1 < 2);  // USBMSG_TYPE5: um_switch_idx=1 < um_size=2

/* Wellspring initialization constants */
#define BCM5974_WELLSPRING_MODE_READ_REQUEST_ID		1
#define BCM5974_WELLSPRING_MODE_WRITE_REQUEST_ID	9

/* Trackpad finger data size, empirically at least ten fingers */
#define MAX_FINGERS		16
#define MAX_FINGER_ORIENTATION	16384

#define BCM5974_MOUSE_SIZE 8

/* trackpad finger structure, le16-aligned */
__declspec(align(2)) struct TRACKPAD_FINGER {
	USHORT origin;		/* zero when switching track finger */
	USHORT abs_x;		/* absolute x coodinate */
	USHORT abs_y;		/* absolute y coodinate */
	USHORT rel_x;		/* relative x coodinate */
	USHORT rel_y;		/* relative y coodinate */
	USHORT tool_major;	/* tool area, major axis */
	USHORT tool_minor;	/* tool area, minor axis */
	USHORT orientation;	/* 16384 when point, else 15 bit angle */
	USHORT touch_major;	/* touch area, major axis */
	USHORT touch_minor;	/* touch area, minor axis */
	USHORT unused[2];	/* zeros */
	USHORT pressure;	/* pressure on forcetouch touchpad */
	USHORT multi;		/* one finger: varies, more fingers: constant */
};

/* device-specific parameters */
struct BCM5974_PARAM {
	int snratio;		/* signal-to-noise ratio */
	int min;			/* device minimum reading */
	int max;			/* device maximum reading */
};

/* device-specific configuration */
struct BCM5974_CONFIG {
	int identification;				/* the product id of this device */
	int caps;						/* device capability bitmask */
	int bt_ep;						/* the endpoint of the button interface */
	int bt_datalen;					/* data length of the button interface */
	int tp_ep;						/* the endpoint of the trackpad interface */
	enum TRACKPAD_TYPE tp_type;		/* type of trackpad interface */
	int tp_header;					/* bytes in header block */
	int tp_datalen;					/* data length of the trackpad interface */
	int tp_button;					/* offset to button data */
	int tp_fsize;					/* bytes in single finger block */
	int tp_delta;					/* offset from header to finger struct */
	int um_size;					/* usb control message length */
	int um_req_val;					/* usb control message value */
	int um_req_idx;					/* usb control message index */
	int um_switch_idx;				/* usb control message mode switch index */
	int um_switch_on;				/* usb control message mode switch on */
	int um_switch_off;				/* usb control message mode switch off */
	struct BCM5974_PARAM p;			/* finger pressure limits */
	struct BCM5974_PARAM w;			/* finger width limits */
	struct BCM5974_PARAM x;			/* horizontal limits */
	struct BCM5974_PARAM y;			/* vertical limits */
	struct BCM5974_PARAM o;			/* orientation limits */
};

#define DATAFORMAT(type)				\
	type,						\
	HEADER_##type,					\
	HEADER_##type + (MAX_FINGERS) * (FSIZE_##type),	\
	BUTTON_##type,					\
	FSIZE_##type,					\
	DELTA_##type,					\
	USBMSG_##type

/* logical signal quality */
#define SN_PRESSURE	45		/* pressure signal-to-noise ratio */
#define SN_WIDTH	25		/* width signal-to-noise ratio */
#define SN_COORD	250		/* coordinate signal-to-noise ratio */
#define SN_ORIENT	10		/* orientation signal-to-noise ratio */

#define PRESSURE_QUALIFICATION_THRESHOLD 2
#define SIZE_QUALIFICATION_THRESHOLD 9
#define SIZE_MU_LOWER_THRESHOLD 5

#define PRESSURE_MU_QUALIFICATION_THRESHOLD_TOTAL 15
#define SIZE_MU_QUALIFICATION_THRESHOLD_TOTAL 25

// ============================================================================
// Keep the normalized coordinate range consistent with the HID descriptor.
// ============================================================================

// The configuration lookup table is defined once in Device.c.
