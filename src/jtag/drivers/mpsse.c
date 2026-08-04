// SPDX-License-Identifier: GPL-2.0-or-later

/**************************************************************************
 *   Copyright (C) 2012 by Andreas Fritiofson                              *
 *   andreas.fritiofson@gmail.com                                          *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mpsse.h"
#include "helper/log.h"
#include "helper/replacements.h"
#include "helper/time_support.h"
#include <ftd2xx.h>

#define DEBUG_PRINT_BUF(buf, len) \
	do { \
		if (LOG_LEVEL_IS(LOG_LVL_DEBUG_IO)) { \
			char buf_string[32 * 3 + 1]; \
			int buf_string_pos = 0; \
			for (int i = 0; i < len; i++) { \
				buf_string_pos += sprintf(buf_string + buf_string_pos, " %02x", buf[i]); \
				if (i % 32 == 32 - 1) { \
					LOG_DEBUG_IO("%s", buf_string); \
					buf_string_pos = 0; \
				} \
			} \
			if (buf_string_pos > 0) \
				LOG_DEBUG_IO("%s", buf_string);\
		} \
	} while (0)

#define FTDI_DEVICE_OUT_REQTYPE (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define FTDI_DEVICE_IN_REQTYPE (0x80 | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)

#define BITMODE_MPSSE 0x02

#define SIO_RESET_REQUEST             0x00
#define SIO_SET_LATENCY_TIMER_REQUEST 0x09
#define SIO_GET_LATENCY_TIMER_REQUEST 0x0A
#define SIO_SET_BITMODE_REQUEST       0x0B

#define SIO_RESET_SIO 0
#define SIO_RESET_PURGE_RX 1
#define SIO_RESET_PURGE_TX 2

struct mpsse_ctx {
    FT_HANDLE usb_dev;
	unsigned int usb_write_timeout;
	unsigned int usb_read_timeout;
	uint8_t in_ep;
	uint8_t out_ep;
	uint16_t max_packet_size;
	uint16_t index;
	uint8_t interf;
	enum ftdi_chip_type type;
	uint8_t *write_buffer;
	unsigned write_size;
	unsigned write_count;
	uint8_t *read_buffer;
	unsigned read_size;
	unsigned read_count;
	uint8_t *read_chunk;
	unsigned read_chunk_size;
	struct bit_copy_queue read_queue;
	int retval;
};

/* Helper to open an ftd2xx device that matches vid, pid, product string and/or serial string.
 * Set any field to 0 as a wildcard. If the device is found true is returned, with ctx containing
 * the already opened handle. ctx->interface must be set to the desired interface (channel) number
 * prior to calling this function. */
static bool open_matching_device(struct mpsse_ctx *ctx, const uint16_t *vid, const uint16_t *pid,
	const char *product, const char *serial, const char *location)
{
    bool found = false;
    static unsigned long prev_id = 0;
    static int chid = 0;
    unsigned long cnt = 0;
    FT_STATUS err = FT_ListDevices(&cnt, NULL, FT_LIST_NUMBER_ONLY);
    if(err != FT_OK) {
        LOG_ERROR("FT_ListDevices() failed with error %d", err);
    }
    FT_DEVICE_LIST_INFO_NODE* device_list = malloc(sizeof(FT_DEVICE_LIST_INFO_NODE) * cnt);
    FT_CreateDeviceInfoList(&cnt);
    err = FT_GetDeviceInfoList(device_list, &cnt);
    if(err != FT_OK) {
        LOG_ERROR("FT_GetDeviceInfoList() failed with error %d", err);
    }

	for (int i = 0; i < cnt; i++) {
	    const FT_DEVICE_LIST_INFO_NODE device = device_list[i];
        const uint16_t device_vid = (device.ID >> 16);
	    const uint16_t device_pid = (device.ID & 0xffff);

		if (vid && *vid != device_vid)
			continue;
		if (pid && *pid != device_pid)
			continue;

        // location is ignored because ftd2xx does not provide that information

		if (product && !strcasecmp(device.Description, product)) {
		    continue;
		}

		if (serial && !strcasecmp(device.SerialNumber, serial)) {
		    continue;
		}

	    err = FT_Open(i, &ctx->usb_dev);
	    if (err != FT_OK) {
	        LOG_ERROR("FT_Open() failed with error %d", err);
	        continue;
	    }

	    switch (device.Type) {
	        case FT_DEVICE_2232C:
	            ctx->type = TYPE_FT2232C;
	            break;
	        case FT_DEVICE_2232H:
	            ctx->type = TYPE_FT2232H;
	            break;
	        case FT_DEVICE_4232H:
	            ctx->type = TYPE_FT4232H;
	            break;
	        case FT_DEVICE_232H:
	            ctx->type = TYPE_FT232H;
	            break;
	        default:
	            LOG_ERROR("unsupported FTDI chip type: 0x%08lx", device.Type);
	            goto error;
	    }

		found = true;
		break;
	}

    free(device_list);

	if (!found) {
		LOG_ERROR("no device found");
		return false;
	}

	// err = libusb_get_config_descriptor(libusb_get_device(ctx->usb_dev), 0, &config0);
	// if (err != LIBUSB_SUCCESS) {
	// 	LOG_ERROR("libusb_get_config_descriptor() failed with %s", libusb_error_name(err));
	// 	libusb_close(ctx->usb_dev);
	// 	return false;
	// }
	//
	/* Make sure the first configuration is selected */
	// int cfg;
	// err = libusb_get_configuration(ctx->usb_dev, &cfg);
	// if (err != LIBUSB_SUCCESS) {
	// 	LOG_ERROR("libusb_get_configuration() failed with %s", libusb_error_name(err));
	// 	goto error;
	// }
	//
	// if (desc.bNumConfigurations > 0 && cfg != config0->bConfigurationValue) {
	// 	err = libusb_set_configuration(ctx->usb_dev, config0->bConfigurationValue);
	// 	if (err != LIBUSB_SUCCESS) {
	// 		LOG_ERROR("libusb_set_configuration() failed with %s", libusb_error_name(err));
	// 		goto error;
	// 	}
	// }

	// /* Try to detach ftdi_sio kernel module */
	// err = libusb_detach_kernel_driver(ctx->usb_dev, ctx->interface);
	// if (err != LIBUSB_SUCCESS && err != LIBUSB_ERROR_NOT_FOUND
	// 		&& err != LIBUSB_ERROR_NOT_SUPPORTED) {
	// 	LOG_WARNING("libusb_detach_kernel_driver() failed with %s, trying to continue anyway",
	// 		libusb_error_name(err));
	// }
	//
	// err = libusb_claim_interface(ctx->usb_dev, ctx->interface);
	// if (err != LIBUSB_SUCCESS) {
	// 	LOG_ERROR("libusb_claim_interface() failed with %s", libusb_error_name(err));
	// 	goto error;
	// }

	/* Reset FTDI device */
	err = FT_ResetDevice(ctx->usb_dev);
	if (err != FT_OK) {
		LOG_ERROR("failed to reset FTDI device with error %lx", err);
		goto error;
	}



	// /* Determine maximum packet size and endpoint addresses */
	// if (!(desc.bNumConfigurations > 0 && ctx->interface < config0->bNumInterfaces
	// 		&& config0->interface[ctx->interface].num_altsetting > 0))
	// 	goto desc_error;
	//
	// const struct libusb_interface_descriptor *descriptor;
	// descriptor = &config0->interface[ctx->interface].altsetting[0];
	// if (descriptor->bNumEndpoints != 2)
	// 	goto desc_error;
	//
	// ctx->in_ep = 0;
	// ctx->out_ep = 0;
	// for (int i = 0; i < descriptor->bNumEndpoints; i++) {
	// 	if (descriptor->endpoint[i].bEndpointAddress & 0x80) {
	// 		ctx->in_ep = descriptor->endpoint[i].bEndpointAddress;
	// 		ctx->max_packet_size =
	// 				descriptor->endpoint[i].wMaxPacketSize;
	// 	} else {
	// 		ctx->out_ep = descriptor->endpoint[i].bEndpointAddress;
	// 	}
	// }
	//
	// if (ctx->in_ep == 0 || ctx->out_ep == 0)
	// 	goto desc_error;
	//
	// libusb_free_config_descriptor(config0);
	return true;

desc_error:
	LOG_ERROR("unrecognized USB device descriptor");
error:
    FT_Close(ctx->usb_dev);
	return false;
}

struct mpsse_ctx *mpsse_open(const uint16_t *vid, const uint16_t *pid, const char *description,
	const char *serial, const char *location, int channel)
{
	struct mpsse_ctx *ctx = calloc(1, sizeof(*ctx));
	FT_STATUS err;

	if (!ctx)
		return nullptr;

	bit_copy_queue_init(&ctx->read_queue);
	ctx->read_chunk_size = 16384;
	ctx->read_size = 16384;
	ctx->write_size = 16384;
	ctx->read_chunk = malloc(ctx->read_chunk_size);
	ctx->read_buffer = malloc(ctx->read_size);

	/* Use calloc to make valgrind happy: buffer_write() sets payload
	 * on bit basis, so some bits can be left uninitialized in write_buffer.
	 * Although this is perfectly ok with MPSSE, valgrind reports
	 * Syscall param ioctl(USBDEVFS_SUBMITURB).buffer points to uninitialised byte(s) */
	ctx->write_buffer = calloc(1, ctx->write_size);

	if (!ctx->read_chunk || !ctx->read_buffer || !ctx->write_buffer)
		goto error;

	ctx->interf = channel;
	ctx->index = channel + 1;
	ctx->usb_read_timeout = 5000;
	ctx->usb_write_timeout = 5000;

	if (!open_matching_device(ctx, vid, pid, description, serial, location)) {
		/* Four hex digits plus terminating zero each */
		char vidstr[5];
		char pidstr[5];
		LOG_ERROR("unable to open ftdi device with vid %s, pid %s, description '%s', "
				"serial '%s'",
				vid ? sprintf(vidstr, "%04x", *vid), vidstr : "*",
				pid ? sprintf(pidstr, "%04x", *pid), pidstr : "*",
				description ? description : "*",
				serial ? serial : "*");
		ctx->usb_dev = nullptr;
		goto error;
	}

    FT_SetTimeouts(ctx->usb_dev, ctx->usb_read_timeout, ctx->usb_write_timeout);
    FT_SetLatencyTimer(ctx->usb_dev, ctx->usb_write_timeout);

    // unsigned long num_bytes_to_read = 0, num_bytes_read_out = 0;
    // err = FT_GetQueueStatus(ctx->usb_dev, &num_bytes_to_read);
    // if (err == FT_OK && num_bytes_to_read > 0) {
    //     FT_Read(ctx->usb_dev, ctx->read_buffer, num_bytes_to_read, &num_bytes_read_out);
    // }
    FT_SetUSBParameters(ctx->usb_dev, ctx->read_size, ctx->write_size);
    FT_SetChars(ctx->usb_dev, FALSE, 0 , FALSE, 0);
    FT_SetBitMode(ctx->usb_dev, 0, 0x00);
    err=FT_SetBitMode(ctx->usb_dev, 0, 0x02);
    if (err != FT_OK) {
        LOG_ERROR("unable to set MPSSE bitmode: %lx", err);
    }
    Sleep(50); // maybe not needed?

	mpsse_purge(ctx);

	return ctx;
error:
	mpsse_close(ctx);
	return nullptr;
}

void mpsse_close(struct mpsse_ctx *ctx)
{
	if (ctx->usb_dev)
		FT_Close(ctx->usb_dev);
	bit_copy_discard(&ctx->read_queue);

	free(ctx->write_buffer);
	free(ctx->read_buffer);
	free(ctx->read_chunk);
	free(ctx);
}

bool mpsse_is_high_speed(struct mpsse_ctx *ctx)
{
	return ctx->type != TYPE_FT2232C;
}

void mpsse_purge(struct mpsse_ctx *ctx)
{
	FT_STATUS err;
	LOG_DEBUG("-");
	ctx->write_count = 0;
	ctx->read_count = 0;
	ctx->retval = ERROR_OK;
	bit_copy_discard(&ctx->read_queue);
	err = FT_Purge(ctx->usb_dev, FT_PURGE_RX);
	if (err != FT_OK) {
		LOG_ERROR("unable to purge ftdi rx buffers: %lx", err);
		return;
	}
    err=FT_Purge(ctx->usb_dev, FT_PURGE_TX);
    if (err != FT_OK) {
        LOG_ERROR("unable to purge ftdi tx buffers: %lx", err);
    }
}

static unsigned buffer_write_space(struct mpsse_ctx *ctx)
{
	/* Reserve one byte for SEND_IMMEDIATE */
	return ctx->write_size - ctx->write_count - 1;
}

static unsigned buffer_read_space(struct mpsse_ctx *ctx)
{
	return ctx->read_size - ctx->read_count;
}

static void buffer_write_byte(struct mpsse_ctx *ctx, uint8_t data)
{
	LOG_DEBUG_IO("%02x", data);
	assert(ctx->write_count < ctx->write_size);
	ctx->write_buffer[ctx->write_count++] = data;
}

static unsigned buffer_write(struct mpsse_ctx *ctx, const uint8_t *out, unsigned out_offset,
	unsigned bit_count)
{
	LOG_DEBUG_IO("%d bits", bit_count);
	assert(ctx->write_count + DIV_ROUND_UP(bit_count, 8) <= ctx->write_size);
	bit_copy(ctx->write_buffer + ctx->write_count, 0, out, out_offset, bit_count);
	ctx->write_count += DIV_ROUND_UP(bit_count, 8);
	return bit_count;
}

static unsigned buffer_add_read(struct mpsse_ctx *ctx, uint8_t *in, unsigned in_offset,
	unsigned bit_count, unsigned offset)
{
	LOG_DEBUG_IO("%d bits, offset %d", bit_count, offset);
	assert(ctx->read_count + DIV_ROUND_UP(bit_count, 8) <= ctx->read_size);
	bit_copy_queued(&ctx->read_queue, in, in_offset, ctx->read_buffer + ctx->read_count, offset,
		bit_count);
	ctx->read_count += DIV_ROUND_UP(bit_count, 8);
	return bit_count;
}

void mpsse_clock_data_out(struct mpsse_ctx *ctx, const uint8_t *out, unsigned out_offset,
	unsigned length, uint8_t mode)
{
	mpsse_clock_data(ctx, out, out_offset, 0, 0, length, mode);
}

void mpsse_clock_data_in(struct mpsse_ctx *ctx, uint8_t *in, unsigned in_offset, unsigned length,
	uint8_t mode)
{
	mpsse_clock_data(ctx, 0, 0, in, in_offset, length, mode);
}

void mpsse_clock_data(struct mpsse_ctx *ctx, const uint8_t *out, unsigned out_offset, uint8_t *in,
	unsigned in_offset, unsigned length, uint8_t mode)
{
	/* TODO: Fix MSB first modes */
	LOG_DEBUG_IO("%s%s %d bits", in ? "in" : "", out ? "out" : "", length);

	if (ctx->retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring command due to previous error");
		return;
	}

	/* TODO: On H chips, use command 0x8E/0x8F if in and out are both 0 */
	if (out || (!out && !in))
		mode |= 0x10;
	if (in)
		mode |= 0x20;

	while (length > 0) {
		/* Guarantee buffer space enough for a minimum size transfer */
		if (buffer_write_space(ctx) + (length < 8) < (out || (!out && !in) ? 4 : 3)
				|| (in && buffer_read_space(ctx) < 1))
			ctx->retval = mpsse_flush(ctx);

		if (length < 8) {
			/* Transfer remaining bits in bit mode */
			buffer_write_byte(ctx, 0x02 | mode);
			buffer_write_byte(ctx, length - 1);
			if (out)
				out_offset += buffer_write(ctx, out, out_offset, length);
			if (in)
				in_offset += buffer_add_read(ctx, in, in_offset, length, 8 - length);
			if (!out && !in)
				buffer_write_byte(ctx, 0x00);
			length = 0;
		} else {
			/* Byte transfer */
			unsigned this_bytes = length / 8;
			/* MPSSE command limit */
			if (this_bytes > 65536)
				this_bytes = 65536;
			/* Buffer space limit. We already made sure there's space for the minimum
			 * transfer. */
			if ((out || (!out && !in)) && this_bytes + 3 > buffer_write_space(ctx))
				this_bytes = buffer_write_space(ctx) - 3;
			if (in && this_bytes > buffer_read_space(ctx))
				this_bytes = buffer_read_space(ctx);

			if (this_bytes > 0) {
				buffer_write_byte(ctx, mode);
				buffer_write_byte(ctx, (this_bytes - 1) & 0xff);
				buffer_write_byte(ctx, (this_bytes - 1) >> 8);
				if (out)
					out_offset += buffer_write(ctx,
							out,
							out_offset,
							this_bytes * 8);
				if (in)
					in_offset += buffer_add_read(ctx,
							in,
							in_offset,
							this_bytes * 8,
							0);
				if (!out && !in)
					for (unsigned n = 0; n < this_bytes; n++)
						buffer_write_byte(ctx, 0x00);
				length -= this_bytes * 8;
			}
		}
	}
}

void mpsse_clock_tms_cs_out(struct mpsse_ctx *ctx, const uint8_t *out, unsigned out_offset,
	unsigned length, bool tdi, uint8_t mode)
{
	mpsse_clock_tms_cs(ctx, out, out_offset, 0, 0, length, tdi, mode);
}

void mpsse_clock_tms_cs(struct mpsse_ctx *ctx, const uint8_t *out, unsigned out_offset, uint8_t *in,
	unsigned in_offset, unsigned length, bool tdi, uint8_t mode)
{
	LOG_DEBUG_IO("%sout %d bits, tdi=%d", in ? "in" : "", length, tdi);
	assert(out);

	if (ctx->retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring command due to previous error");
		return;
	}

	mode |= 0x42;
	if (in)
		mode |= 0x20;

	while (length > 0) {
		/* Guarantee buffer space enough for a minimum size transfer */
		if (buffer_write_space(ctx) < 3 || (in && buffer_read_space(ctx) < 1))
			ctx->retval = mpsse_flush(ctx);

		/* Byte transfer */
		unsigned this_bits = length;
		/* MPSSE command limit */
		/* NOTE: there's a report of an FT2232 bug in this area, where shifting
		 * exactly 7 bits can make problems with TMS signaling for the last
		 * clock cycle:
		 *
		 * http://developer.intra2net.com/mailarchive/html/libftdi/2009/msg00292.html
		 */
		if (this_bits > 7)
			this_bits = 7;

		if (this_bits > 0) {
			buffer_write_byte(ctx, mode);
			buffer_write_byte(ctx, this_bits - 1);
			uint8_t data = 0;
			/* TODO: Fix MSB first, if allowed in MPSSE */
			bit_copy(&data, 0, out, out_offset, this_bits);
			out_offset += this_bits;
			buffer_write_byte(ctx, data | (tdi ? 0x80 : 0x00));
			if (in)
				in_offset += buffer_add_read(ctx,
						in,
						in_offset,
						this_bits,
						8 - this_bits);
			length -= this_bits;
		}
	}
}

void mpsse_set_data_bits_low_byte(struct mpsse_ctx *ctx, uint8_t data, uint8_t dir)
{
	LOG_DEBUG_IO("-");

	if (ctx->retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring command due to previous error");
		return;
	}

	if (buffer_write_space(ctx) < 3)
		ctx->retval = mpsse_flush(ctx);

	buffer_write_byte(ctx, 0x80);
	buffer_write_byte(ctx, data);
	buffer_write_byte(ctx, dir);
}

void mpsse_set_data_bits_high_byte(struct mpsse_ctx *ctx, uint8_t data, uint8_t dir)
{
	LOG_DEBUG_IO("-");

	if (ctx->retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring command due to previous error");
		return;
	}

	if (buffer_write_space(ctx) < 3)
		ctx->retval = mpsse_flush(ctx);

	buffer_write_byte(ctx, 0x82);
	buffer_write_byte(ctx, data);
	buffer_write_byte(ctx, dir);
}

void mpsse_read_data_bits_low_byte(struct mpsse_ctx *ctx, uint8_t *data)
{
	LOG_DEBUG_IO("-");

	if (ctx->retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring command due to previous error");
		return;
	}

	if (buffer_write_space(ctx) < 1 || buffer_read_space(ctx) < 1)
		ctx->retval = mpsse_flush(ctx);

	buffer_write_byte(ctx, 0x81);
	buffer_add_read(ctx, data, 0, 8, 0);
}

void mpsse_read_data_bits_high_byte(struct mpsse_ctx *ctx, uint8_t *data)
{
	LOG_DEBUG_IO("-");

	if (ctx->retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring command due to previous error");
		return;
	}

	if (buffer_write_space(ctx) < 1 || buffer_read_space(ctx) < 1)
		ctx->retval = mpsse_flush(ctx);

	buffer_write_byte(ctx, 0x83);
	buffer_add_read(ctx, data, 0, 8, 0);
}

static void single_byte_boolean_helper(struct mpsse_ctx *ctx, bool var, uint8_t val_if_true,
	uint8_t val_if_false)
{
	if (ctx->retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring command due to previous error");
		return;
	}

	if (buffer_write_space(ctx) < 1)
		ctx->retval = mpsse_flush(ctx);

	buffer_write_byte(ctx, var ? val_if_true : val_if_false);
}

void mpsse_loopback_config(struct mpsse_ctx *ctx, bool enable)
{
	LOG_DEBUG("%s", enable ? "on" : "off");
	single_byte_boolean_helper(ctx, enable, 0x84, 0x85);
}

void mpsse_set_divisor(struct mpsse_ctx *ctx, uint16_t divisor)
{
	LOG_DEBUG("%d", divisor);

	if (ctx->retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring command due to previous error");
		return;
	}

	if (buffer_write_space(ctx) < 3)
		ctx->retval = mpsse_flush(ctx);

	buffer_write_byte(ctx, 0x86);
	buffer_write_byte(ctx, divisor & 0xff);
	buffer_write_byte(ctx, divisor >> 8);
}

int mpsse_divide_by_5_config(struct mpsse_ctx *ctx, bool enable)
{
	if (!mpsse_is_high_speed(ctx))
		return ERROR_FAIL;

	LOG_DEBUG("%s", enable ? "on" : "off");
	single_byte_boolean_helper(ctx, enable, 0x8b, 0x8a);

	return ERROR_OK;
}

int mpsse_rtck_config(struct mpsse_ctx *ctx, bool enable)
{
	if (!mpsse_is_high_speed(ctx))
		return ERROR_FAIL;

	LOG_DEBUG("%s", enable ? "on" : "off");
	single_byte_boolean_helper(ctx, enable, 0x96, 0x97);

	return ERROR_OK;
}

int mpsse_set_frequency(struct mpsse_ctx *ctx, int frequency)
{
	LOG_DEBUG("target %d Hz", frequency);
	assert(frequency >= 0);
	int base_clock;

	if (frequency == 0)
		return mpsse_rtck_config(ctx, true);

	mpsse_rtck_config(ctx, false); /* just try */

	if (frequency > 60000000 / 2 / 65536 && mpsse_divide_by_5_config(ctx, false) == ERROR_OK) {
		base_clock = 60000000;
	} else {
		mpsse_divide_by_5_config(ctx, true); /* just try */
		base_clock = 12000000;
	}

	int divisor = (base_clock / 2 + frequency - 1) / frequency - 1;
	if (divisor > 65535)
		divisor = 65535;
	assert(divisor >= 0);

	mpsse_set_divisor(ctx, divisor);

	frequency = base_clock / 2 / (1 + divisor);
	LOG_DEBUG("actually %d Hz", frequency);

	return frequency;
}

/* Context needed by the callbacks */
struct transfer_result {
	struct mpsse_ctx *ctx;
	bool done;
	unsigned long transferred;
};

int mpsse_flush(struct mpsse_ctx *ctx)
{
	int retval = ctx->retval;
    FT_STATUS err;

	if (retval != ERROR_OK) {
		LOG_DEBUG_IO("Ignoring flush due to previous error");
		assert(ctx->write_count == 0 && ctx->read_count == 0);
		ctx->retval = ERROR_OK;
		return retval;
	}

	LOG_DEBUG_IO("write %d%s, read %d", ctx->write_count, ctx->read_count ? "+1" : "",
			ctx->read_count);
	assert(ctx->write_count > 0 || ctx->read_count == 0); /* No read data without write data */

	if (ctx->write_count == 0)
		return retval;

	struct transfer_result read_result = { .ctx = ctx, .done = true };
	if (ctx->read_count) {
		buffer_write_byte(ctx, 0x87); /* SEND_IMMEDIATE */
		read_result.done = false;
		/* delay read transaction to ensure the FTDI chip can support us with data
		   immediately after processing the MPSSE commands in the write transaction */
	}

	struct transfer_result write_result = { .ctx = ctx, .done = false };
    unsigned long num_bytes_written;
    err = FT_Write(ctx->usb_dev, ctx->write_buffer, ctx->write_count, &write_result.transferred);
    write_result.done = true;
    if (err != FT_OK) {
        goto error_check;
    }

    if (ctx -> read_count) {
        err = FT_Read(ctx->usb_dev, ctx->read_buffer, ctx->read_count, &read_result.transferred);
        read_result.done=true;
        if (err != FT_OK) {
            goto error_check;
        }
    }

	/* Polling loop, more or less taken from libftdi */
	int64_t start = timeval_ms();
	int64_t warn_after = 2000;

error_check:
    if (write_result.transferred < ctx->write_count) {
		LOG_ERROR("ftdi device did not accept all data: %d, tried %d",
			write_result.transferred,
			ctx->write_count);
		retval = ERROR_FAIL;
	} else if (read_result.transferred < ctx->read_count) {
		LOG_ERROR("ftdi device did not return all data: %d, expected %d",
			read_result.transferred,
			ctx->read_count);
		retval = ERROR_FAIL;
	} else if (ctx->read_count) {
		ctx->write_count = 0;
		ctx->read_count = 0;
		bit_copy_execute(&ctx->read_queue);
		retval = ERROR_OK;
	} else {
		ctx->write_count = 0;
		bit_copy_discard(&ctx->read_queue);
		retval = ERROR_OK;
	}

	if (retval != ERROR_OK)
		mpsse_purge(ctx);

	return retval;
}
