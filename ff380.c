// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ff380.c — Linux kernel driver for the Trust FF380 RaceMaster (06D6:005F)
 *
 * The wheel uses a pure vendor USB protocol with no HID PID support.
 * Force feedback is handled by the kernel's ff-memless layer:
 * all effect types (constant, spring, damper, periodic, ramp, …) are
 * evaluated and mixed by the kernel and delivered here as a single signed
 * byte in effect->u.ramp.start_level (-128 … +127, X axis).
 *
 * OUT endpoint 0x02 — 8-byte interrupt packets:
 *
 *   Set force:   11 FF FF <force> 00 00 00 00
 *   Commit:      01 11 00 00      00 00 00 00   (must follow every set-force)
 *   Reset/center:05 01 00 00      00 00 00 00
 *
 * Force byte encoding (byte index 3):
 *   0x00       = zero force
 *   0x01…0x7F  = right, increasing strength
 *   0x80…0xFF  = left,  increasing strength  (0x80 = weakest, 0xFF = strongest)
 *
 * Tested encoder (from reverse-engineering on Windows):
 *   f > 0  ->  (uint8_t) clamp(f, 0, 127) right force
 *   f < 0  ->  (uint8_t)(127 - clamp(f, -128, 0)) = 127..255 left force
 *   f = 0  ->  0x00 zero force
 *
 * IN endpoint 0x81 — 8-byte interrupt packets:
 *   [0-1]  padding / unused
 *   [2]    steering axis  (0-255)
 *   [3]    ADC common / unused
 *   [4]    right pedal / gas   (0-255)
 *   [5]    left pedal / brake  (0-255)
 *   [6]    button group B (bitfield, 8 buttons)
 *   [7]    button group A (D-pad hat switch, bits 0x0-0x07 clockwise, 0xFF = neutral)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/usb/input.h>

MODULE_AUTHOR("ff380 driver");
MODULE_DESCRIPTION("Trust FF380 RaceMaster force-feedback driver");
MODULE_LICENSE("GPL");

/* ── USB IDs ─────────────────────────────────────────────────────────────── */

#define FF380_VENDOR_ID  0x06D6
#define FF380_PRODUCT_ID 0x005F

/* ── USB endpoints ───────────────────────────────────────────────────────── */

#define FF380_EP_IN   0x81   /* interrupt IN  – wheel state reports */
#define FF380_EP_OUT  0x02   /* interrupt OUT – force commands       */

/* Polling interval for the IN URB (ms -> used only as timeout elsewhere) */
#define FF380_POLL_MS  10

/* ── USB OUT packet sizes ─────────────────────────────────────────────────── */

#define FF380_PKT_LEN  8

/* ── Per-device state ───────────────────────────────────────────────────────*/

struct ff380_device {
	struct usb_device *udev;
	struct input_dev  *idev;

	/* IN URB – continuous polling for wheel position / buttons */
	struct urb *in_urb;
	u8         *in_buf;
	dma_addr_t  in_dma;

	/* OUT buffer – protected by out_lock, used from ff callback */
	spinlock_t  out_lock;

	/* Physical path string for input_dev */
	char phys[64];

	/* Last known states so we only emit on changes */
	u8 last_btn_a;
	u8 last_btn_b;
	u8 last_force_byte;
};

/* ── Force encoding ──────────────────────────────────────────────────────── */
/*
 * Maps a signed value (-128…+127) to the vendor's force byte.
 *
 * The kernel ff-memless layer delivers the mixed force in
 * effect->u.ramp.start_level  (type s16, but clamped to s8 range by kernel)
 *
 * Vendor encoding:
 *   0x00        = no force
 *   0x01…0x7F  = force right  (1 = weakest … 0x7F = strongest)
 *   0x80…0xFF  = force left   (0x80 = weakest … 0xFF = strongest)
 */
static u8 ff380_encode_force(s8 f)
{
	if (f == -128)
        f = -127;

	f = -f; // invert: kernel left = vendor right, kernel right = vendor left
	if (f > 0)
		return (u8)f;
	if (f < 0)
		return (u8)(127 - f); // 127 - (-1)=128=0x80 … 127-(-128)=255=0xFF
	return 0;
}

/* ── USB OUT helpers (called from ff callback, may be in softirq context) ── */

/* Forward declaration – the work struct lives inside the device state */
struct ff380_device;

/* Work item submitted to send force over USB */
struct ff380_force_work {
	struct work_struct  work;
	struct ff380_device *ff380;
	u8                  force_byte;
};

/* We embed one statically allocated work item in the device state.
 * If a new force arrives before the previous work ran we just update
 * force_byte in place (worst case we send one extra packet). */
struct ff380_dev_full {
	struct ff380_device         dev;
	struct ff380_force_work     fw;
	struct workqueue_struct    *wq;
};

#define ff380_full(d)  container_of((d), struct ff380_dev_full, dev)

static void ff380_force_work_fn(struct work_struct *work)
{
	struct ff380_force_work *fw =
		container_of(work, struct ff380_force_work, work);
	struct ff380_device *ff380 = fw->ff380;
	u8 force_byte = READ_ONCE(fw->force_byte);

	if (force_byte == READ_ONCE(ff380->last_force_byte))
        return;

	WRITE_ONCE(ff380->last_force_byte, force_byte);

	int actual;

	/* --- set force packet --- */
	u8 pkt[FF380_PKT_LEN] = {0x11, 0xFF, 0xFF, force_byte, 0, 0, 0, 0};

	int r = usb_interrupt_msg(ff380->udev,
				  usb_sndintpipe(ff380->udev, FF380_EP_OUT),
				  pkt, FF380_PKT_LEN,
				  &actual, FF380_POLL_MS * 3);
	if (r && r != -ENODEV)
		pr_debug("set force error %d\n", r);

	/* --- commit packet --- */
	u8 commit[FF380_PKT_LEN] = {0x01, 0x11, 0x00, 0x00, 0, 0, 0, 0};

	r = usb_interrupt_msg(ff380->udev,
			      usb_sndintpipe(ff380->udev, FF380_EP_OUT),
			      commit, FF380_PKT_LEN,
			      &actual, FF380_POLL_MS * 3);
	if (r && r != -ENODEV)
		pr_debug("commit error %d\n", r);
}

static void ff380_send_force_async(struct ff380_device *ff380, u8 force_byte)
{
	struct ff380_dev_full *full = ff380_full(ff380);

	WRITE_ONCE(full->fw.force_byte, force_byte);
	queue_work(full->wq, &full->fw.work);
}

/* ── ff-memless callback ─────────────────────────────────────────────────── */
/*
 * The kernel mixes all active effects and calls us here (from a kernel timer,
 * i.e. softirq context).  The combined result is packed into a FF_CONSTANT
 * effect where:
 *   effect->u.ramp.start_level = X force component  (s8 range, -128…+127)
 *   effect->u.ramp.end_level   = Y force component  (unused for a wheel)
 *
 * effect->type will always be FF_CONSTANT when called from ff-memless.
 * A value of 0 means "stop all forces".
 */
static int ff380_play_effect(struct input_dev *idev, void *data, struct ff_effect *effect)
{
	struct ff380_device *ff380 = input_get_drvdata(idev);
	s8 level;

	// pr_info("ff380_play_effect called: type=%d\n", effect->type);

	if (effect->type == FF_CONSTANT) {
		/*
		 * ff-memless stores the mixed X component in ramp.start_level.
		 * The value is already clamped to s8 range (-128…+127) by the
		 * kernel's ml_combine_effects().
		 */
		level = (s8)effect->u.ramp.start_level;
		// pr_info("ff380: ramp.start=%d ramp.end=%d\n", effect->u.ramp.start_level, effect->u.ramp.end_level);
	} else {
		pr_info("ff380: unexpected type %d\n", effect->type);
		level = 0;
	}

	u8 force_byte = ff380_encode_force(level);
	// pr_info("ff380: force_byte=0x%02X\n", force_byte);
	ff380_send_force_async(ff380, force_byte);
	return 0;
}

/* ── IN URB completion — wheel reports position, pedals, buttons ─────────── */

static void ff380_in_urb_complete(struct urb *urb)
{
	struct ff380_device *ff380 = urb->context;
	struct input_dev    *idev  = ff380->idev;
	u8 *buf = ff380->in_buf;
	int i, r;

	switch (urb->status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return;   /* device disconnected or URB killed */
	default:
		pr_debug("IN URB status %d\n", urb->status);
		goto resubmit;
	}

	if (urb->actual_length < 8)
		goto resubmit;

	/*
	 * Byte layout:
	 *  [2]  steering  0-255 -> ABS_X
	 *  [4]  gas       0-255 -> ABS_GAS
	 *  [5]  brake     0-255 -> ABS_BRAKE
	 *  [6]  button group B (D-pad hat switch, bits 0x0-0x07 clockwise, 0xFF = neutral)
	 *  [7]  button group A (bits 0-7 -> BTN_JOYSTICK+8 … BTN_JOYSTICK+15)
	 */

	/* Steering, gas, brake */
	input_report_abs(idev, ABS_X, buf[2]);
	input_report_abs(idev, ABS_GAS, buf[4]);
	input_report_abs(idev, ABS_BRAKE, buf[5]);

	/* Button group A */
	if (buf[7] != ff380->last_btn_a) {
		for (i = 0; i < 8; i++) {
			bool cur  = (buf[7] >> i) & 1;
			bool prev = (ff380->last_btn_a >> i) & 1;
			if (cur != prev)
				input_report_key(idev, BTN_JOYSTICK + i, cur);
		}
		ff380->last_btn_a = buf[7];
	}

	/* Button group B (D-pad) */
	if (buf[6] != ff380->last_btn_b) {
	    static const s8 hat_x[9] = { 0,  1,  1,  1,  0, -1, -1, -1,  0 };
	    static const s8 hat_y[9] = {-1, -1,  0,  1,  1,  1,  0, -1,  0 };
	    u8 hat = buf[6];
	    int idx = (hat <= 7) ? hat : 8;   /* 0xFF -> 8 = neutral */
	    input_report_abs(idev, ABS_HAT0X, hat_x[idx]);
	    input_report_abs(idev, ABS_HAT0Y, hat_y[idx]);
	    ff380->last_btn_b = buf[6];
	}

	input_sync(idev);

resubmit:
	r = usb_submit_urb(urb, GFP_ATOMIC);
	if (r && r != -ENODEV)
		pr_err("IN URB resubmit failed: %d\n", r);
}

/* ── probe / disconnect ──────────────────────────────────────────────────── */

static int ff380_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device      *udev = interface_to_usbdev(intf);
	struct ff380_dev_full  *full;
	struct ff380_device    *ff380;
	struct input_dev       *idev;
	struct usb_endpoint_descriptor *ep_in = NULL;
	struct usb_host_interface      *iface_desc;
	int i, r;

	/* Find the interrupt IN endpoint */
	iface_desc = intf->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep =
			&iface_desc->endpoint[i].desc;
		if (usb_endpoint_is_int_in(ep)) {
			ep_in = ep;
			break;
		}
	}

	if (!ep_in) {
		dev_err(&intf->dev, "No interrupt IN endpoint found\n");
		return -ENODEV;
	}

	/* Allocate combined state struct */
	full = kzalloc(sizeof(*full), GFP_KERNEL);
	if (!full)
		return -ENOMEM;

	ff380 = &full->dev;
	ff380->last_force_byte = 0xFF; // random force byte so first force packet will be sent
	ff380->udev = usb_get_dev(udev);
	spin_lock_init(&ff380->out_lock);

	/* Physical path string */
	usb_make_path(udev, ff380->phys, sizeof(ff380->phys));
	strlcat(ff380->phys, "/input0", sizeof(ff380->phys));

	/* Workqueue for async USB OUT (ff callback runs in softirq) */
	full->wq = alloc_ordered_workqueue("ff380_ff", WQ_MEM_RECLAIM);
	if (!full->wq) {
		r = -ENOMEM;
		goto err_free_full;
	}
	full->fw.ff380 = ff380;
	INIT_WORK(&full->fw.work, ff380_force_work_fn);

	/* Allocate IN URB */
	ff380->in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ff380->in_urb) {
		r = -ENOMEM;
		goto err_destroy_wq;
	}

	ff380->in_buf = usb_alloc_coherent(udev, FF380_PKT_LEN,
					   GFP_KERNEL, &ff380->in_dma);
	if (!ff380->in_buf) {
		r = -ENOMEM;
		goto err_free_in_urb;
	}

	/* Set up the input device */
	idev = devm_input_allocate_device(&intf->dev);
	if (!idev) {
		r = -ENOMEM;
		goto err_free_in_buf;
	}

	ff380->idev   = idev;
	idev->name    = "Trust FF380 RaceMaster";
	idev->phys    = ff380->phys;
	idev->uniq    = "";   /* no serial number */
	idev->id.bustype = BUS_USB;
	idev->id.vendor  = FF380_VENDOR_ID;
	idev->id.product = FF380_PRODUCT_ID;
	idev->id.version = 1;
	idev->dev.parent = &intf->dev;

	input_set_drvdata(idev, ff380);

	/* Axes */
	set_bit(EV_ABS, idev->evbit);
	input_set_abs_params(idev, ABS_X, 0, 255, 2, 4);   /* steering */
	input_set_abs_params(idev, ABS_GAS, 0, 255, 2, 4);   /* gas      */
	input_set_abs_params(idev, ABS_BRAKE, 0, 255, 2, 4);   /* brake    */

	/* Buttons — group A only (8 real buttons) */
	set_bit(EV_KEY, idev->evbit);
	for (i = 0; i < 8; i++)
	    set_bit(BTN_JOYSTICK + i, idev->keybit);

	/* D-pad as hat switch */
	input_set_abs_params(idev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(idev, ABS_HAT0Y, -1, 1, 0, 0);

	/* Force feedback — let ff-memless handle all effect types */
	set_bit(EV_FF, idev->evbit);

	/*
	 * Advertise the effect types the wheel physically supports.
	 * ff-memless will evaluate and mix all of these for us.
	 */
	set_bit(FF_CONSTANT,  idev->ffbit);
	set_bit(FF_SPRING,    idev->ffbit);
	set_bit(FF_DAMPER,    idev->ffbit);
	set_bit(FF_FRICTION,  idev->ffbit);
	set_bit(FF_INERTIA,   idev->ffbit);
	set_bit(FF_RAMP,      idev->ffbit);
	set_bit(FF_PERIODIC,  idev->ffbit);
	set_bit(FF_SINE,      idev->ffbit);
	set_bit(FF_SQUARE,    idev->ffbit);
	set_bit(FF_TRIANGLE,  idev->ffbit);
	set_bit(FF_SAW_UP,    idev->ffbit);
	set_bit(FF_SAW_DOWN,  idev->ffbit);
	set_bit(FF_GAIN,      idev->ffbit);

	r = input_ff_create_memless(idev, NULL, ff380_play_effect);
	if (r) {
		dev_err(&intf->dev, "input_ff_create_memless failed: %d\n", r);
		goto err_free_in_buf;
	}

	/* Set up and submit the IN URB */
	usb_fill_int_urb(ff380->in_urb, udev,
			 usb_rcvintpipe(udev, ep_in->bEndpointAddress),
			 ff380->in_buf, FF380_PKT_LEN,
			 ff380_in_urb_complete, ff380,
			 ep_in->bInterval);
	ff380->in_urb->transfer_dma    = ff380->in_dma;
	ff380->in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	r = input_register_device(idev);
	if (r) {
		dev_err(&intf->dev, "input_register_device failed: %d\n", r);
		goto err_free_in_buf;
	}

	r = usb_submit_urb(ff380->in_urb, GFP_KERNEL);
	if (r) {
		dev_err(&intf->dev, "IN URB submit failed: %d\n", r);
	}

	usb_set_intfdata(intf, full);

	/* Send an initial zero-force + reset so the wheel is centered */
	{
		u8 reset[FF380_PKT_LEN] = {0x05, 0x01, 0, 0, 0, 0, 0, 0};
		int actual;
		usb_interrupt_msg(udev, usb_sndintpipe(udev, FF380_EP_OUT),
				  reset, FF380_PKT_LEN, &actual, 100);
	}

	dev_info(&intf->dev, "Trust FF380 RaceMaster connected\n");
	return 0;

err_free_in_buf:
	usb_free_coherent(udev, FF380_PKT_LEN, ff380->in_buf, ff380->in_dma);
err_free_in_urb:
	usb_free_urb(ff380->in_urb);
err_destroy_wq:
	destroy_workqueue(full->wq);
err_free_full:
	usb_put_dev(udev);
	kfree(full);
	return r;
}

static void ff380_disconnect(struct usb_interface *intf)
{
	struct ff380_dev_full *full = usb_get_intfdata(intf);
	struct ff380_device   *ff380;

	if (!full)
		return;

	ff380 = &full->dev;
	usb_set_intfdata(intf, NULL);

	/* Stop the IN URB */
	usb_kill_urb(ff380->in_urb);

	/* Stop all pending force work */
	destroy_workqueue(full->wq);

	/* Send reset before leaving */
	{
		u8 reset[FF380_PKT_LEN] = {0x05, 0x01, 0, 0, 0, 0, 0, 0};
		u8 commit[FF380_PKT_LEN] = {0x01, 0x11, 0, 0, 0, 0, 0, 0};
		int actual;
		usb_interrupt_msg(ff380->udev,
				  usb_sndintpipe(ff380->udev, FF380_EP_OUT),
				  reset,   FF380_PKT_LEN, &actual, 100);
		usb_interrupt_msg(ff380->udev,
				  usb_sndintpipe(ff380->udev, FF380_EP_OUT),
				  commit, FF380_PKT_LEN, &actual, 100);
	}

	usb_free_coherent(ff380->udev, FF380_PKT_LEN,
			  ff380->in_buf, ff380->in_dma);
	usb_free_urb(ff380->in_urb);
	usb_put_dev(ff380->udev);
	kfree(full);

	dev_info(&intf->dev, "Trust FF380 RaceMaster disconnected\n");
}

/* ── USB device table and driver registration ────────────────────────────── */

static const struct usb_device_id ff380_id_table[] = {
	{ USB_DEVICE(FF380_VENDOR_ID, FF380_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, ff380_id_table);

static struct usb_driver ff380_driver = {
	.name       = "ff380",
	.probe      = ff380_probe,
	.disconnect = ff380_disconnect,
	.id_table   = ff380_id_table,
};

module_usb_driver(ff380_driver);
