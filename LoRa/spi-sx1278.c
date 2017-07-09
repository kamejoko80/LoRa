/*-
 * Copyright (c) 2017 Jian-Hong, Pan <starnight@g.ncu.edu.tw>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    similar to the "NO WARRANTY" disclaimer below ("Disclaimer") and any
 *    redistribution must be conditioned upon including a substantially
 *    similar Disclaimer requirement for further binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ''AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF NONINFRINGEMENT, MERCHANTIBILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGES.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/compat.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <asm/div64.h>

#include "lora.h"
#include "sx1278.h"

#ifndef F_XOSC
#define F_XOSC		32000000
#endif
#define	__POW_2_19	0x80000

/*------------------------------ SPI Functions -------------------------------*/

/**
 * sx127X_sync - Do the SPI communication with the device
 * @spi:	spi device to communicate with
 * @m:		spi message going to be transferred
 *
 * Return:	How many bytes has been transferred, -1 for failed
 */
ssize_t
sx127X_sync(struct spi_device *spi, struct spi_message *m)
{
	DECLARE_COMPLETION_ONSTACK(done);
	int status;

	if (spi == NULL)
		status = -ESHUTDOWN;
	else
		status = spi_sync(spi, m);

	if (status == 0)
		status = m->actual_length;

	return status;
}

/**
 * sx127X_read_reg - Build SPI read message and read from the SPI device
 * @spi:	spi device to communicate with
 * @adr:	the register's start address which is going to be read from
 * @buf:	the buffer going to be read into, from the registers
 * @len:	the length of the buffer in bytes
 *
 * Return:	How many bytes has been read, -1 for failed
 */
int
sx127X_read_reg(struct spi_device *spi, uint8_t adr, void *buf, size_t len)
{
	int status = 0;
	struct spi_transfer at, bt;
	struct spi_message m;

	spi_message_init(&m);

	/* Read address.  The MSB must be 0 because of reading an address. */
	memset(&at, 0, sizeof(at));
	at.tx_buf = &adr;
	at.len = 1;
	spi_message_add_tail(&at, &m);

	/* Read value. */
	memset(&bt, 0, sizeof(bt));
	bt.rx_buf = buf;
	bt.len = len;
	spi_message_add_tail(&bt, &m);

	status = sx127X_sync(spi, &m);
	/* Minus the start address's length. */
	if (status > 0)
		status -= 1;

	return status;
}

/**
 * sx127X_write_reg - Build SPI write message and write into the SPI device
 * @spi:	spi device to communicate with
 * @adr:	the register's start address which is going to be written into
 * @buf:	the buffer going to be written into the registers
 * @len:	the length of the buffer in bytes
 *
 * Return:	How many bytes has been written, -1 for failed
 */
int
sx127X_write_reg(struct spi_device *spi, uint8_t adr, void *buf, size_t len)
{
	int status = 0;
	struct spi_transfer at, bt;
	struct spi_message m;

	spi_message_init(&m);

	/* Write address.  The MSB must be 1 because of writing an address. */
	adr |= 0x80;
	memset(&at, 0, sizeof(at));
	at.tx_buf = &adr;
	at.len = 1;
	spi_message_add_tail(&at, &m);

	/* Write value. */
	memset(&bt, 0, sizeof(bt));
	bt.tx_buf = buf;
	bt.len = len;
	spi_message_add_tail(&bt, &m);

	status = sx127X_sync(spi, &m);
	/* Minus the start address's length. */
	if (status > 0)
		status -= 1;

	return status;
}

/*------------------------------ LoRa Functions ------------------------------*/

/**
 * sx127X_readVersion - Get LoRa device's chip version
 * @spi:	spi device to communicate with
 * @vstr:	the buffer going to hold the chip's version string
 * @len:	the max length of the buffer
 *
 * Return:	Positive / negtive values for version code / failed
 * 		Version code:	bits 7-4 full version number,
 * 				bits 3-0 metal mask revision number
 */
int
sx127X_readVersion(struct spi_device *spi)
{
	uint8_t v;
	int status;

	status = sx127X_read_reg(spi, SX127X_REG_VERSION, &v, 1);

	if ((status == 1) && (0 < v) && (v < 0xFF))
		status = v;
	else
		status = -ENODEV;

	return status;
}

/**
 * sx127X_getMode - Get LoRa device's mode register
 * @spi:	spi device to communicate with
 *
 * Return:	LoRa device's register value
 */
uint8_t 
sx127X_getMode(struct spi_device *spi)
{
	uint8_t op_mode;

	/* Get original OP Mode register. */
	sx127X_read_reg(spi, SX127X_REG_OP_MODE, &op_mode, 1);

	return op_mode;
}

/**
 * sx127X_getState - Set LoRa device's operating state
 * @spi:	spi device to communicate with
 * @st:		LoRa device's operating state going to be assigned
 */
void
sx127X_setState(struct spi_device *spi, uint8_t st)
{
	uint8_t op_mode;

	/* Get original OP Mode register. */
	op_mode = sx127X_getMode(spi);
	/* Set device to designated state. */
	op_mode = (op_mode & 0xF8) | (st & 0x07);
	sx127X_write_reg(spi, SX127X_REG_OP_MODE, &op_mode, 1);
}

/**
 * sx127X_getState - Get LoRa device's operating state
 * @spi:	spi device to communicate with
 *
 * Return:	LoRa device's operating state
 */
uint8_t
sx127X_getState(struct spi_device *spi)
{
	uint8_t op_mode;

	op_mode = sx127X_getMode(spi) & 0x07;

	return op_mode;
}

/**
 * sx127X_getLoRaFreq - Set RF frequency
 * @spi:	spi device to communicate with
 * @fr:		RF frequency going to be assigned in Hz
 */
void
sx127X_setLoRaFreq(struct spi_device *spi, uint32_t fr)
{
	uint64_t frt64;
	uint32_t frt;
	uint8_t buf[3];
	int i;
	uint32_t f_xosc;

#ifdef CONFIG_OF
	/* Set the LoRa module's crystal oscillator's clock if OF is defined. */
	const void *ptr;

	ptr = of_get_property(spi->dev.of_node, "clock-frequency", NULL);
	f_xosc = (ptr != NULL) ? be32_to_cpup(ptr) : F_XOSC;
#else
	f_xosc = F_XOSC;
#endif

	frt64 = (uint64_t)fr * (uint64_t)__POW_2_19;
	do_div(frt64, f_xosc);
	frt = frt64;

	for (i = 2; i >= 0; i--) {
		buf[i] = frt % 256;
		frt = frt >> 8;
	}

	sx127X_write_reg(spi, SX127X_REG_FRF_MSB, buf, 3);
}

/**
 * sx127X_getLoRaFreq - Get RF frequency
 * @spi:	spi device to communicate with
 *
 * Return:	RF frequency in Hz
 */
uint32_t
sx127X_getLoRaFreq(struct spi_device *spi)
{
	uint64_t frt = 0;
	uint8_t buf[3];
	int status;
	int i;
	uint32_t fr;
	uint32_t f_xosc;

#ifdef CONFIG_OF
	/* Set the LoRa module's crystal oscillator's clock if OF is defined. */
	const void *ptr;

	ptr = of_get_property(spi->dev.of_node, "clock-frequency", NULL);
	f_xosc = (ptr != NULL) ? be32_to_cpup(ptr) : F_XOSC;
#else
	f_xosc = F_XOSC;
#endif

	status = sx127X_read_reg(spi, SX127X_REG_FRF_MSB, buf, 3);
	if (status <= 0)
		return 0.0;

	for (i = 0; i <= 2; i++)
		frt = frt * 256 + buf[i];

	fr =  frt * f_xosc / __POW_2_19;

	return fr;
}

/**
 * sx127X_setLoRaPower - Set RF output power
 * @spi:	spi device to communicate with
 * @pout:	RF output power going to be assigned in dbm
 */
void
sx127X_setLoRaPower(struct spi_device *spi, int32_t pout)
{
	uint8_t pac;
	uint8_t boost;
	uint8_t outputPower;
	int32_t pmax;

	if (pout > 15) {
		/* Pout > 15dbm */
		boost = 1;
		pmax = 7;
		outputPower = pout - 2;
	}
	else if (pout < 0) {
		/* Pout < 0dbm */
		boost = 0;
		pmax = 2;
		outputPower = 3 + pout;
	}
	else {
		/* 0dbm <= Pout <= 15dbm */
		boost = 0;
		pmax = 7;
		outputPower = pout;
	}

	pac = (boost << 7) | (pmax << 4) | (outputPower);
	sx127X_write_reg(spi, SX127X_REG_PA_CONFIG, &pac, 1);
}

/**
 * sx127X_getLoRaPower - Get RF output power
 * @spi:	spi device to communicate with
 *
 * Return:	RF output power in dbm
 */
int32_t
sx127X_getLoRaPower(struct spi_device *spi)
{
	uint8_t pac;
	uint8_t boost;
	int32_t outputPower;
	int32_t pmax;
	int32_t pout;

	sx127X_read_reg(spi, SX127X_REG_PA_CONFIG, &pac, 1);
	boost = (pac & 0x80) >> 7;
	outputPower = pac & 0x0F;
	if (boost) {
		pout = 2 + outputPower;
	}
	else {
		/* Power max should be pmax/10.  It is 10 times for now. */
		pmax = (108 + 6 * ((pac & 0x70) >> 4));
		pout = (pmax - (150 - outputPower * 10)) / 10;
	}

	return pout;
}

int8_t lna_gain[] = {
	 0,
	-6,
	-12,
	-24,
	-26,
	-48
};

/**
 * sx127X_setLoRaLNA - Set RF LNA gain
 * @spi:	spi device to communicate with
 * @db:		RF LNA gain going to be assigned in db
 */
void
sx127X_setLoRaLNA(struct spi_device *spi, int32_t db)
{
	uint8_t i, g;
	uint8_t lnacf;

	for (i = 0; i < 5; i++) {
		if (lna_gain[i] <= db)
			break;
	}
	g = i + 1;

	sx127X_read_reg(spi, SX127X_REG_LNA, &lnacf, 1);
	lnacf = (lnacf & 0x1F) | (g << 5);
	sx127X_write_reg(spi, SX127X_REG_LNA, &lnacf, 1);
}

/**
 * sx127X_getLoRaLNA - Get RF LNA gain
 * @spi:	spi device to communicate with
 *
 * Return:	RF LNA gain db
 */
int32_t
sx127X_getLoRaLNA(struct spi_device *spi)
{
	int32_t db;
	int8_t i, g;
	uint8_t lnacf;

	sx127X_read_reg(spi, SX127X_REG_LNA, &lnacf, 1);
	g = (lnacf >> 5);
	i = g - 1;
	db = lna_gain[i];

	return db;
}

/**
 * sx127X_setLoRaLNAAGC - Set RF LNA go with auto gain control or not
 * @spi:	spi device to communicate with
 * @yesno:	1 / 0 for auto gain control / manual
 */
void
sx127X_setLoRaLNAAGC(struct spi_device *spi, int32_t yesno)
{
	uint8_t mcf3;

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG3, &mcf3, 1);
	mcf3 = (yesno) ? (mcf3 | 0x04) : (mcf3 & (~0x04));
	sx127X_write_reg(spi, SX127X_REG_MODEM_CONFIG3, &mcf3, 1);
}

/**
 * sx127X_getLoRaAllFlag - Get all of the LoRa device's IRQ flags' current state
 * @spi:	spi device to communicate with
 *
 * Return:	All of the LoRa device's IRQ flags' current state in a byte
 */
uint8_t
sx127X_getLoRaAllFlag(struct spi_device *spi)
{
	uint8_t flags;

	sx127X_read_reg(spi, SX127X_REG_IRQ_FLAGS, &flags, 1);

	return flags;
}

/**
 * sx127X_getLoRaAllFlag - Get interested LoRa device's IRQ flag's current state
 * @spi:	spi device to communicate with
 * @f:		the interested LoRa device's IRQ flag
 *
 * Return:	The interested LoRa device's IRQ flag's current state in a byte
 */
#define sx127X_getLoRaFlag(spi, f)	(sx127X_getLoRaAllFlag(spi) & (f))

/**
 * sx127X_clearLoRaFlag - Clear designated LoRa device's IRQ flag
 * @spi:	spi device to communicate with
 * @f:		flags going to be cleared
 */
void
sx127X_clearLoRaFlag(struct spi_device *spi, uint8_t f)
{
	uint8_t flag;

	/* Get oiginal flag. */
	flag = sx127X_getLoRaAllFlag(spi);
	/* Set the designated bits of the flag. */
	flag |= f;
	sx127X_write_reg(spi, SX127X_REG_IRQ_FLAGS, &flag, 1);
}

/**
 * sx127X_clearLoRaAllFlag - Clear designated LoRa device's all IRQ flags
 * @spi:	spi device to communicate with
 */
#define sx127X_clearLoRaAllFlag(spi)	sx127X_clearLoRaFlag(spi, 0xFF)

/**
 * sx127X_getLoRaSPRFactor - Get the RF modulation's spreading factor
 * @spi:	spi device to communicate with
 * @c_s:	Spreading factor in chips / symbol
 */
void
sx127X_setLoRaSPRFactor(struct spi_device *spi, uint32_t c_s)
{
	uint8_t sf;
	uint8_t mcf2;

	for (sf = 6; sf < 12; sf++) {
		if (c_s == ((uint32_t)1 << sf))
			break;
	}

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG2, &mcf2, 1);
	mcf2 = (mcf2 & 0x0F) | (sf << 4);
	sx127X_write_reg(spi, SX127X_REG_MODEM_CONFIG2, &mcf2, 1);
}

/**
 * sx127X_getLoRaSPRFactor - Get the RF modulation's spreading factor
 * @spi:	spi device to communicate with
 *
 * Return:	Spreading factor in chips / symbol
 */
uint32_t
sx127X_getLoRaSPRFactor(struct spi_device *spi)
{
	uint8_t sf;
	uint32_t c_s;

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG2, &sf, 1);
	sf = sf >> 4;
	c_s = 1 << sf;

	return c_s;
}

const uint32_t hz[] = {
	  7800,
	 10400,
	 15600,
	 20800,
	 31250,
	 41700,
	 62500,
	125000,
	250000,
	500000
};

/**
 * sx127X_getLoRaBW - Set RF bandwidth
 * @spi:	spi device to communicate with
 * @bw:		RF bandwidth going to be assigned in Hz
 */
void
sx127X_setLoRaBW(struct spi_device *spi, uint32_t bw)
{
	uint8_t i;
	uint8_t mcf1;

	for (i = 0; i < 9; i++) {
		if (hz[i] >= bw)
			break;
	}

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG1, &mcf1, 1);
	mcf1 = (mcf1 & 0x0F) | (i << 4);
	sx127X_write_reg(spi, SX127X_REG_MODEM_CONFIG1, &mcf1, 1);
}

/**
 * sx127X_getLoRaBW - Get RF bandwidth
 * @spi:	spi device to communicate with
 *
 * Return:	RF bandwidth in Hz
 */
uint32_t
sx127X_getLoRaBW(struct spi_device *spi)
{
	uint8_t mcf1;
	uint8_t bw;

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG1, &mcf1, 1);
	bw = mcf1 >> 4;

	return hz[bw];
}

/**
 * sx127X_setLoRaCR  - Get LoRa package's coding rate
 * @spi:	spi device to communicate with
 * @cr:		Coding rate going to be assigned in a byte
 * 		high 4 bits / low 4 bits: numerator / denominator
 */
void
sx127X_setLoRaCR(struct spi_device *spi, uint8_t cr)
{
	uint8_t mcf1;

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG1, &mcf1, 1);
	mcf1 = (mcf1 & 0x0E) | (((cr & 0xF) - 4) << 1);
	sx127X_write_reg(spi, SX127X_REG_MODEM_CONFIG1, &mcf1, 1);
}

/**
 * sx127X_getLoRaCR - Get LoRa package's coding rate
 * @spi:	spi device to communicate with
 *
 * Return:	Coding rate in a byte
 * 		high 4 bits / low 4 bits: numerator / denominator
 */
uint8_t
sx127X_getLoRaCR(struct spi_device *spi)
{
	uint8_t mcf1;
	uint8_t cr;	/* ex: 0x45 represents cr=4/5 */

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG1, &mcf1, 1);
	cr = 0x40 + ((mcf1 & 0x0E) >> 1) + 4;
	
	return cr;
}

/**
 * sx127X_setLoRaImplicit - Set LoRa packages in Explicit / Implicit Header Mode
 * @spi:	spi device to communicate with
 * @yesno:	1 / 0 for Implicit Header Mode / Explicit Header Mode
 */
void
sx127X_setLoRaImplicit(struct spi_device *spi, uint8_t yesno)
{
	uint8_t mcf1;

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG1, &mcf1, 1);
	mcf1 = (yesno) ? (mcf1 | 0x01) : (mcf1 & 0xFE);
	sx127X_write_reg(spi, SX127X_REG_MODEM_CONFIG1, &mcf1, 1);
}

/**
 * sx127X_setLoRaRXByteTimeout - Get RX operation time-out in terms of symbols
 * @spi:	spi device to communicate with
 * @n:		Time-out in terms of symbols (bytes) going to be assigned
 */
void
sx127X_setLoRaRXByteTimeout(struct spi_device *spi, uint32_t n)
{
	uint8_t buf[2];
	uint8_t mcf2;

	if (n < 1)
		n = 1;
	if (n > 1023)
		n = 1023;

	/* Read original Modem config 2. */
	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG2, &mcf2, 1);

	/* LSB */
	buf[1] = n % 256;
	/* MSB */
	buf[0] = (mcf2 & 0xFC) | (n >> 8);

	sx127X_write_reg(spi, SX127X_REG_MODEM_CONFIG2, buf, 2);
}

/**
 * sx127X_setLoRaRXTimeout - Set RX operation time-out seconds
 * @spi:	spi device to communicate with
 * @ms:		The RX time-out time in ms
 */
void
sx127X_setLoRaRXTimeout(struct spi_device *spi, uint32_t ms)
{
	uint32_t n;

	n = ms * sx127X_getLoRaBW(spi) / (sx127X_getLoRaSPRFactor(spi) * 1000);

	sx127X_setLoRaRXByteTimeout(spi, n);
}

/**
 * sx127X_getLoRaRXByteTimeout - Get RX operation time-out in terms of symbols
 * @spi:	spi device to communicate with
 *
 * Return:	Time-out in terms of symbols (bytes)
 */
uint32_t
sx127X_getLoRaRXByteTimeout(struct spi_device *spi)
{
	uint32_t n;
	uint8_t buf[2];

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG2, buf, 2);

	n = (buf[0] & 0x03) * 256 + buf[1];

	return n;
}

/**
 * sx127X_getLoRaRXTimeout - Get RX operation time-out seconds
 * @spi:	spi device to communicate with
 *
 * Return:	The RX time-out time in ms
 */
uint32_t
sx127X_getLoRaRXTimeout(struct spi_device *spi)
{
	uint32_t ms;

	ms = 1000 * sx127X_getLoRaRXByteTimeout(spi) * \
		sx127X_getLoRaSPRFactor(spi) / sx127X_getLoRaBW(spi);

	return ms;
}

/**
 * sx127X_setLoRaMaxRXBuff - Maximum payload length in LoRa packet
 * @spi:	spi device to communicate with
 * @len:	the max payload length going to be assigned in bytes
 */
void
sx127X_setLoRaMaxRXBuff(struct spi_device *spi, uint8_t len)
{
	sx127X_write_reg(spi, SX127X_REG_MAX_PAYLOAD_LENGTH, &len, 1);
}

/**
 * sx127X_readLoRaData - Read data from LoRa device (RX)
 * @spi:	spi device to communicate with
 * @buf:	buffer going to be read data into
 * @len:	the length of the data going to be read in bytes
 *
 * Return:	the actual data length read from the LoRa device in bytes
 */
ssize_t
sx127X_readLoRaData(struct spi_device *spi, uint8_t *buf, size_t len)
{
	uint8_t start_adr;
	uint8_t blen;
	ssize_t c;

	/* Get the chip RX FIFO last packet address. */
	sx127X_read_reg(spi, SX127X_REG_FIFO_RX_CURRENT_ADDR, &start_adr, 1);
	/* Set chip FIFO pointer to FIFO last packet address. */
	sx127X_write_reg(spi, SX127X_REG_FIFO_ADDR_PTR, &start_adr, 1);

	/* Get the RX last packet payload length. */
	sx127X_read_reg(spi, SX127X_REG_RX_NB_BYTES, &blen, 1);

	len = (blen < len) ? blen : len;
	/* Read LoRa packet payload. */
	c = sx127X_read_reg(spi, SX127X_REG_FIFO, buf, len);

	return c;
}

/**
 * sx127X_sendLoRaData - Send data out through LoRa device (TX)
 * @spi:	spi device to communicate with
 * @buf:	buffer going to be send
 * @len:	the length of the buffer in bytes
 *
 * Return:	the actual length written into the LoRa device in bytes
 */
ssize_t
sx127X_sendLoRaData(struct spi_device *spi, uint8_t *buf, size_t len)
{
	uint8_t base_adr;
	uint8_t blen;
	ssize_t c;

	/* Set chip FIFO pointer to FIFO TX base. */
	sx127X_read_reg(spi, SX127X_REG_FIFO_TX_BASE_ADDR, &base_adr, 1);
	sx127X_write_reg(spi, SX127X_REG_FIFO_ADDR_PTR, &base_adr, 1);

#define SX127X_MAX_FIFO_LENGTH	0xFF
	blen = (len < SX127X_MAX_FIFO_LENGTH) ? len : SX127X_MAX_FIFO_LENGTH;

	/* Write to SPI chip synchronously to fill the FIFO of the chip. */
	c = sx127X_write_reg(spi, SX127X_REG_FIFO, buf, blen);

	/* Set the FIFO payload length. */
	blen = c;
	sx127X_write_reg(spi, SX127X_REG_PAYLOAD_LENGTH, &blen, 1);

	return c;
}

/**
 * sx127X_getLoRaLastPacketSNR - Get last LoRa packet's SNR
 * @spi:	spi device to communicate with
 *
 * Return:	the last LoRa packet's SNR in db
 */
int32_t
sx127X_getLoRaLastPacketSNR(struct spi_device *spi)
{
	int32_t db;
	int8_t snr;

	sx127X_read_reg(spi, SX127X_REG_PKT_SNR_VALUE, &snr, 1);
	db = snr / 4;

	return db;
}

/**
 * sx127X_getLoRaLastPacketRSSI - Get last LoRa packet's SNR
 * @spi:	spi device to communicate with
 *
 * Return:	the last LoRa packet's RSSI in dbm
 */
int32_t
sx127X_getLoRaLastPacketRSSI(struct spi_device *spi)
{
	int32_t dbm;
	uint8_t lhf;
	uint8_t rssi;
	int8_t snr;

	/* Get LoRa is in high or low frequency mode. */
	lhf = sx127X_getMode(spi) & 0x08;
	/* Get RSSI value. */
	sx127X_read_reg(spi, SX127X_REG_PKT_RSSI_VALUE, &rssi, 1);
	dbm = (lhf) ? -164 + rssi : -157 + rssi;

	/* Adjust to correct the last packet RSSI if SNR < 0. */
	sx127X_read_reg(spi, SX127X_REG_PKT_SNR_VALUE, &snr, 1);
	if(snr < 0)
		dbm += snr / 4;

	return dbm;
}

/**
 * sx127X_getLoRaRSSI - Get current RSSI value
 * @spi:	spi device to communicate with
 *
 * Return:	the current RSSI in dbm
 */
int32_t
sx127X_getLoRaRSSI(struct spi_device *spi)
{
	int32_t dbm;
	uint8_t lhf;
	uint8_t rssi;

	/* Get LoRa is in high or low frequency mode. */
	lhf = sx127X_getMode(spi) & 0x08;
	/* Get RSSI value. */
	sx127X_read_reg(spi, SX127X_REG_RSSI_VALUE, &rssi, 1);
	dbm = (lhf) ? -164 + rssi : -157 + rssi;

	return dbm;
}

/**
 * sx127X_setLoRaPreambleLen - Set LoRa preamble length
 * @spi:	spi device to communicate with
 * @len:	the preamble length going to be assigned
 */
void
sx127X_setLoRaPreambleLen(struct spi_device *spi, uint32_t len)
{
	uint8_t pl[2];

	pl[1] = len % 256;
	pl[0] = (len >> 8) % 256;

	sx127X_write_reg(spi, SX127X_REG_PREAMBLE_MSB, pl, 2);
}

/**
 * sx127X_getLoRaPreambleLen - Get LoRa preamble length
 * @spi:	spi device to communicate with
 *
 * Return:	length of the LoRa preamble
 */
uint32_t
sx127X_getLoRaPreambleLen(struct spi_device *spi)
{
	uint8_t pl[2];
	uint32_t len;

	sx127X_read_reg(spi, SX127X_REG_PREAMBLE_MSB, pl, 2);
	len = pl[0] * 256 + pl[1];

	return len;
}

/**
 * sx127X_setLoRaCRC - Enable CRC generation and check on received payload
 * @spi:	spi device to communicate with
 * @yesno:	1 / 0 for check / not check
 */
void
sx127X_setLoRaCRC(struct spi_device *spi, uint8_t yesno)
{
	uint8_t mcf2;

	sx127X_read_reg(spi, SX127X_REG_MODEM_CONFIG2, &mcf2, 1);
	mcf2 = (yesno) ? mcf2 | (1 << 2) : mcf2 & (~(1 << 2));
	sx127X_write_reg(spi, SX127X_REG_MODEM_CONFIG2, &mcf2, 1);
}

/**
 * sx127X_setBoost - Set RF power amplifier boost in normal output range
 * @spi:	spi device to communicate with
 * @yesno:	1 / 0 for boost / not boost
 */
void
sx127X_setBoost(struct spi_device *spi, uint8_t yesno)
{
	uint8_t pacf;

	sx127X_read_reg(spi, SX127X_REG_PA_CONFIG, &pacf, 1);
	pacf = (yesno) ? pacf | (1 << 7) : pacf & (~(1 << 7));
	sx127X_write_reg(spi, SX127X_REG_PA_CONFIG, &pacf, 1);
}

/**
 * sx127X_startLoRaMode - Start the device and set it in LoRa mode
 * @spi:	spi device to communicate with
 */
void
sx127X_startLoRaMode(struct spi_device *spi)
{
	uint8_t op_mode;
	uint8_t base_adr;

	/* Get original OP Mode register. */
	op_mode = sx127X_getMode(spi);
	dev_dbg(&(spi->dev), "the original OP mode is 0x%X\n", op_mode);
	/* Set device to sleep state. */
	sx127X_setState(spi, SX127X_SLEEP_MODE);
	/* Set device to LoRa mode. */
	op_mode = sx127X_getMode(spi);
	op_mode = op_mode | 0x80;
	sx127X_write_reg(spi, SX127X_REG_OP_MODE, &op_mode, 1);
	/* Set device to standby state. */
	sx127X_setState(spi, SX127X_STANDBY_MODE);
	op_mode = sx127X_getMode(spi);
	dev_dbg(&(spi->dev), "the current OP mode is 0x%X\n", op_mode);

	/* Set LoRa in explicit header mode. */
	sx127X_setLoRaImplicit(spi, 0);

	/* Set chip FIFO RX base. */
	base_adr = 0x00;
	dev_dbg(&(spi->dev), "going to set RX base address\n");
	sx127X_write_reg(spi, SX127X_REG_FIFO_RX_BASE_ADDR, &base_adr, 1);
	sx127X_write_reg(spi, SX127X_REG_FIFO_ADDR_PTR, &base_adr, 1);

	/* Clear all of the IRQ flags. */
	sx127X_clearLoRaAllFlag(spi);
	/* Set chip to RX continuous state waiting for receiving. */
	sx127X_setState(spi, SX127X_RXCONTINUOUS_MODE);
}

/**
 * init_sx127X - Initial the SX127X device
 * @spi:	spi device to communicate with
 *
 * Return:	Positive / negtive values for version code / failed
 * 		Version code:	bits 7-4 full version number,
 * 				bits 3-0 metal mask revision number
 */
int
init_sx127X(struct spi_device *spi)
{
	int v;
	uint8_t fv, mmv;

	dev_dbg(&(spi->dev), "init sx127X\n");

	v = sx127X_readVersion(spi);
	if (v > 0) {
		fv = (v >> 4) & 0xF;
		mmv = v & 0xF;
		dev_dbg(&(spi->dev), "chip version %d.%d\n", fv, mmv);

		sx127X_startLoRaMode(spi);
	}

	return v;
}

/*---------------------------- LoRa SPI Functions ----------------------------*/

#define __DRIVER_NAME		"sx1278"
#ifndef N_LORASPI_MINORS
#define N_LORASPI_MINORS	8
#endif

static DECLARE_BITMAP(minors, N_LORASPI_MINORS);

static DEFINE_MUTEX(minors_lock);

/**
 * loraspi_read - Read from the LoRa device's communication
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the read data in user space
 * @size:	the length of the buffer in bytes
 *
 * Return:	Read how many bytes actually, negative number for error
 */
static ssize_t
loraspi_read(struct lora_struct *lrdata, const char __user *buf, size_t size)
{
	struct spi_device *spi;
	ssize_t status;
	int c = 0;
	uint8_t adr;
	uint8_t flag;
	uint8_t st;
	uint32_t timeout;

	spi = lrdata->lora_device;
	dev_dbg(&(spi->dev), "Read %zu bytes into user space\n", size);

	mutex_lock(&(lrdata->buf_lock));
	/* Get chip's current state. */
	st = sx127X_getState(spi);

	/*  Prepare and set the chip to RX continuous mode, if it is not. */
	if (st != SX127X_RXCONTINUOUS_MODE) {
		/* Set chip to standby state. */
		dev_dbg(&(spi->dev), "Going to set standby state\n");
		sx127X_setState(spi, SX127X_STANDBY_MODE);

		/* Set chip FIFO RX base. */
		adr = 0x00;
		dev_dbg(&(spi->dev), "Going to set RX base address\n");
		sx127X_write_reg(spi, SX127X_REG_FIFO_RX_BASE_ADDR, &adr, 1);

		/* Clear all of the IRQ flags. */
		sx127X_clearLoRaAllFlag(spi);
		/* Set chip to RX continuous state waiting for receiving. */
		sx127X_setState(spi, SX127X_RXCONTINUOUS_MODE);
	}

	/* Wait and check there is any packet received ready. */
	for (timeout = 0; timeout < 250; timeout++) {
		flag = sx127X_getLoRaFlag(spi,
					SX127X_FLAG_RXTIMEOUT |
					SX127X_FLAG_RXDONE |
					SX127X_FLAG_PAYLOADCRCERROR);
		if (flag == 0)
			msleep(20);
		else
			break;
	}

	/* If there is nothing or received timeout. */
	if ((flag == 0) || (flag & SX127X_FLAG_RXTIMEOUT)) {
		c = -ENODATA;
	}
	/* If there is a packet, but the payload is CRC error. */
	if (sx127X_getLoRaFlag(spi, SX127X_FLAG_PAYLOADCRCERROR)) {
		c = -EBADMSG;
	}

	/* There is a ready packet in the chip's FIFO. */
	if (c == 0) {
		memset(lrdata->rx_buf, 0, lrdata->bufmaxlen);
		size = (lrdata->bufmaxlen < size) ? lrdata->bufmaxlen : size;
		/* Read from chip to LoRa data RX buffer. */
		c = sx127X_readLoRaData(spi, lrdata->rx_buf, size);
		/* Copy from LoRa data RX buffer to user space. */
		if (c > 0)
			status = copy_to_user((void *)buf, lrdata->rx_buf, c);
	}

	/* Clear all of the IRQ flags. */
	sx127X_clearLoRaAllFlag(spi);

	mutex_unlock(&(lrdata->buf_lock));

	return c;
}

/**
 * loraspi_write - Write to the LoRa device's communication
 * @lrdata:	LoRa device
 * @arg:	the buffer holding the data going to be written in user space
 * @size:	the length of the buffer in bytes
 *
 * Return:	Write how many bytes actually, negative number for error
 */
static ssize_t
loraspi_write(struct lora_struct *lrdata, const char __user *buf, size_t size)
{
	struct spi_device *spi;
	ssize_t status;
	int c;
	uint8_t adr;
	uint8_t flag;
	uint32_t timeout;

	spi = lrdata->lora_device;
	dev_dbg(&(spi->dev), "Write %zu bytes from user space\n", size);

	mutex_lock(&(lrdata->buf_lock));
	memset(lrdata->tx_buf, 0, lrdata->bufmaxlen);
	status = copy_from_user(lrdata->tx_buf, buf, size);

	if (status >= size)
		return 0;

	lrdata->tx_buflen = size - status;

	/* Set chip to standby state. */
	dev_dbg(&(spi->dev), "Going to set standby state\n");
	sx127X_setState(spi, SX127X_STANDBY_MODE);

	/* Set chip FIFO TX base. */
	adr = 0x80;
	dev_dbg(&(spi->dev), "Going to set TX base address\n");
	sx127X_write_reg(spi, SX127X_REG_FIFO_TX_BASE_ADDR, &adr, 1);

	/* Write to SPI chip synchronously to fill the FIFO of the chip. */
	c = sx127X_sendLoRaData(spi, lrdata->tx_buf, lrdata->tx_buflen);

	/* Clear LoRa IRQ TX flag. */
	sx127X_clearLoRaFlag(spi, SX127X_FLAG_TXDONE);

	if (c > 0) {
		/* Set chip to TX state to send the data in FIFO to RF. */
		dev_dbg(&(spi->dev), "Set TX state\n");
		sx127X_setState(spi, SX127X_TX_MODE);

		timeout = (c + sx127X_getLoRaPreambleLen(spi) + 1) + 2;
		dev_dbg(&(spi->dev), "The time out is %u ms", timeout * 20);

		/* Wait until TX is finished by checking the TX flag. */
		for (flag = 0; timeout > 0; timeout--) {
			flag = sx127X_getLoRaFlag(spi, SX127X_FLAG_TXDONE);
			if (flag != 0) {
				dev_dbg(&(spi->dev), "Wait TX is finished\n");
				break;
			}

			if (timeout == 1) {
				c = 0;
				dev_dbg(&(spi->dev), "Wait TX is time out\n");
			}
			else {
				msleep(20);
			}
		}
	}

	/* Set chip to RX continuous state. */
	dev_dbg(&(spi->dev), "Set back to RX continuous state\n");
	sx127X_setState(spi, SX127X_STANDBY_MODE);
	sx127X_setState(spi, SX127X_RXCONTINUOUS_MODE);

	lrdata->tx_buflen = 0;

	mutex_unlock(&(lrdata->buf_lock));

	return c;
}

/**
 * loraspi_setstate - Set the state of the LoRa device
 * @lrdata:	LoRa device
 * @arg:	the buffer holding the state value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_setstate(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t st32;
	uint8_t st;

	spi = lrdata->lora_device;
	status = copy_from_user(&st32, arg, sizeof(uint32_t));
	switch (st32) {
	case LORA_STATE_SLEEP:
		st = SX127X_SLEEP_MODE;		break;
	case LORA_STATE_STANDBY:
		st = SX127X_STANDBY_MODE;	break;
	case LORA_STATE_TX:
		st = SX127X_TX_MODE;		break;
	case LORA_STATE_RX:
		st = SX127X_RXCONTINUOUS_MODE;	break;
	case LORA_STATE_CAD:
		st = SX127X_CAD_MODE;		break;
	default:
		st = SX127X_STANDBY_MODE;
	}

	mutex_lock(&(lrdata->buf_lock));
	sx127X_setState(spi, st);
	mutex_unlock(&(lrdata->buf_lock));

	return 0;
}

/**
 * loraspi_getstate - Get the state of the LoRa device
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the state value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_getstate(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t st32;
	uint8_t st;

	spi = lrdata->lora_device;

	mutex_lock(&(lrdata->buf_lock));
	st = sx127X_getState(spi);
	mutex_unlock(&(lrdata->buf_lock));

	st32 = st;
	switch (st) {
	case SX127X_SLEEP_MODE:
		st32 = LORA_STATE_SLEEP;	break;
	case SX127X_STANDBY_MODE:
		st32 = LORA_STATE_STANDBY;	break;
	case SX127X_FSTX_MODE:
	case SX127X_TX_MODE:
		st32 = LORA_STATE_TX;		break;
	case SX127X_FSRX_MODE:
	case SX127X_RXSINGLE_MODE:
	case SX127X_RXCONTINUOUS_MODE:
		st32 = LORA_STATE_RX;		break;
	case SX127X_CAD_MODE:
		st32 = LORA_STATE_CAD;		break;
	default:
		st32 = LORA_STATE_SLEEP;
	}
	status = copy_to_user(arg, &st32, sizeof(uint32_t));

	return 0;
}

/**
 * loraspi_setfreq - Set the carrier frequency
 * @lrdata:	LoRa device
 * @arg:	the buffer holding the carrier frequency in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_setfreq(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t freq;

	spi = lrdata->lora_device;
	status = copy_from_user(&freq, arg, sizeof(uint32_t));
	dev_dbg(&(spi->dev), "Set frequency %u Hz from user space\n", freq);

	mutex_lock(&(lrdata->buf_lock));
	sx127X_setLoRaFreq(spi, freq);
	mutex_unlock(&(lrdata->buf_lock));

	return 0;
}

/**
 * loraspi_getfreq - Get the carrier frequency
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the carrier frequency in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_getfreq(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t freq;

	spi = lrdata->lora_device;
	dev_dbg(&(spi->dev), "Get frequency to user space\n");

	mutex_lock(&(lrdata->buf_lock));
	freq = sx127X_getLoRaFreq(spi);
	mutex_unlock(&(lrdata->buf_lock));
	dev_dbg(&(spi->dev), "The carrier freq is %u Hz\n", freq);

	status = copy_to_user(arg, &freq, sizeof(uint32_t));

	return 0;
}

/**
 * loraspi_setpower - Set the PA power
 * @lrdata:	LoRa device
 * @arg:	the buffer holding the PA output value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_setpower(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	int32_t dbm;

	spi = lrdata->lora_device;
	status = copy_from_user(&dbm, arg, sizeof(int32_t));

#define LORA_MAX_POWER	(17)
#define LORA_MIN_POWER	(-2)
	if (dbm > LORA_MAX_POWER)
		dbm = LORA_MAX_POWER;
	else if (dbm < LORA_MIN_POWER)
		dbm = LORA_MIN_POWER;

	mutex_lock(&(lrdata->buf_lock));
	sx127X_setLoRaPower(spi, dbm);
	mutex_unlock(&(lrdata->buf_lock));

	return 0;
}

/**
 * loraspi_getpower -  Get the PA power
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the PA output value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_getpower(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	int32_t dbm;

	spi = lrdata->lora_device;

	mutex_lock(&(lrdata->buf_lock));
	dbm = sx127X_getLoRaPower(spi);
	mutex_unlock(&(lrdata->buf_lock));

	status = copy_to_user(arg, &dbm, sizeof(int32_t));

	return 0;
}

/**
 * loraspi_setLNA - Set the LNA gain
 * @lrdata:	LoRa device
 * @arg:	the buffer holding the LNA gain value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_setLNA(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	int32_t db;

	spi = lrdata->lora_device;
	status = copy_from_user(&db, arg, sizeof(int32_t));

#define LORA_MAX_LNA	(0)
#define LORA_MIN_LNA	(-48)
	if (db > LORA_MAX_LNA)
		db = LORA_MAX_LNA;
	else if (db < LORA_MIN_LNA)
		db = LORA_MIN_LNA;

	mutex_lock(&(lrdata->buf_lock));
	sx127X_setLoRaLNA(spi, db);
	mutex_unlock(&(lrdata->buf_lock));

	return 0;
}

/**
 * loraspi_getLNA -  Get the LNA gain
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the LNA gain value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_getLNA(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	int32_t db;

	spi = lrdata->lora_device;

	mutex_lock(&(lrdata->buf_lock));
	db = sx127X_getLoRaLNA(spi);
	mutex_unlock(&(lrdata->buf_lock));

	status = copy_to_user(arg, &db, sizeof(int32_t));

	return 0;
}

/**
 * loraspi_setLNAAGC - Set the LNA be auto gain control
 * @lrdata:	LoRa device
 * @arg:	the buffer holding the auto gain control or not in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_setLNAAGC(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t agc;

	spi = lrdata->lora_device;
	status = copy_from_user(&agc, arg, sizeof(uint32_t));

	agc = (agc == 1) ? 1 : 0;
	mutex_lock(&(lrdata->buf_lock));
	sx127X_setLoRaLNAAGC(spi, agc);
	mutex_unlock(&(lrdata->buf_lock));

	return 0;
}

/**
 * loraspi_setsprfactor - Set the RF spreading factor
 * @lrdata:	LoRa device
 * @arg:	the buffer holding the spreading factor in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_setsprfactor(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t sprf;

	spi = lrdata->lora_device;
	status = copy_from_user(&sprf, arg, sizeof(uint32_t));

	mutex_lock(&(lrdata->buf_lock));
	sx127X_setLoRaSPRFactor(spi, sprf);
	mutex_unlock(&(lrdata->buf_lock));

	return 0;
}

/**
 * loraspi_getsprfactor - Get the RF spreading factor
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the spreading factor in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_getsprfactor(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t sprf;

	spi = lrdata->lora_device;

	mutex_lock(&(lrdata->buf_lock));
	sprf = sx127X_getLoRaSPRFactor(spi);
	mutex_unlock(&(lrdata->buf_lock));

	status = copy_to_user(arg, &sprf, sizeof(uint32_t));

	return 0;
}

/**
 * loraspi_setbandwidth - Set the RF bandwith
 * @lrdata:	LoRa device
 * @arg:	the buffer holding the RF bandwith value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_setbandwidth(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t bw;

	spi = lrdata->lora_device;
	status = copy_from_user(&bw, arg, sizeof(uint32_t));

	mutex_lock(&(lrdata->buf_lock));
	sx127X_setLoRaBW(spi, bw);
	mutex_unlock(&(lrdata->buf_lock));

	return 0;
}

/**
 * loraspi_getbandwidth - Get the RF bandwith
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the RF bandwith value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_getbandwidth(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	uint32_t bw;

	spi = lrdata->lora_device;

	mutex_lock(&(lrdata->buf_lock));
	bw = sx127X_getLoRaBW(spi);
	mutex_unlock(&(lrdata->buf_lock));

	status = copy_to_user(arg, &bw, sizeof(uint32_t));

	return 0;
}

/**
 * loraspi_getrssi - Get current RSSI
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the RSSI value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_getrssi(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	int32_t rssi;

	spi = lrdata->lora_device;

	mutex_lock(&(lrdata->buf_lock));
	rssi = sx127X_getLoRaRSSI(spi);
	mutex_unlock(&(lrdata->buf_lock));

	status = copy_to_user(arg, &rssi, sizeof(int32_t));

	return 0;
}

/**
 * loraspi_getsnr - Get last packet's SNR
 * @lrdata:	LoRa device
 * @arg:	the buffer going to hold the SNR value in user space
 *
 * Return:	0 / other values for success / error
 */
static long
loraspi_getsnr(struct lora_struct *lrdata, void __user *arg)
{
	struct spi_device *spi;
	int status;
	int32_t snr;

	spi = lrdata->lora_device;

	mutex_lock(&(lrdata->buf_lock));
	snr = sx127X_getLoRaLastPacketSNR(spi);
	mutex_unlock(&(lrdata->buf_lock));

	status = copy_to_user(arg, &snr, sizeof(int32_t));

	return 0;
}

/**
 * loraspi_ready2write - Is ready to be written
 * @lrdata:	LoRa device
 *
 * Return:	1 / 0 for ready / not ready
 */
static long
loraspi_ready2write(struct lora_struct *lrdata)
{
	long ret;

	/* Mutex is not lock, than it is not writing. */
	ret = mutex_is_locked(&(lrdata->buf_lock)) ? 0 : 1;

	return ret;
}

/**
 * loraspi_ready2read - Is ready to be read
 * @lrdata:	LoRa device
 *
 * Return:	1 / 0 for ready / not ready
 */
static long
loraspi_ready2read(struct lora_struct *lrdata)
{
	struct spi_device *spi;
	long ret;

	spi = lrdata->lora_device;

	ret = 0;
	/* Mutex is not lock, than it is not in reading file operation. */
	if (!mutex_is_locked(&(lrdata->buf_lock))) {
		/* Check the chip have recieved full data. */
		mutex_lock(&(lrdata->buf_lock));
		ret = sx127X_getLoRaFlag(spi, SX127X_FLAG_RXDONE) != 0;
		mutex_unlock(&(lrdata->buf_lock));
	}

	return ret;
}

struct lora_driver lr_driver = {
	.name = __DRIVER_NAME,
	.num = N_LORASPI_MINORS,
	.owner = THIS_MODULE,
};

struct lora_operations lrops = {
	.read = loraspi_read,
	.write = loraspi_write,
	.setState = loraspi_setstate,
	.getState = loraspi_getstate,
	.setFreq = loraspi_setfreq,
	.getFreq = loraspi_getfreq,
	.setPower = loraspi_setpower,
	.getPower = loraspi_getpower,
	.setLNA = loraspi_setLNA,
	.getLNA = loraspi_getLNA,
	.setLNAAGC = loraspi_setLNAAGC,
	.setSPRFactor = loraspi_setsprfactor,
	.getSPRFactor = loraspi_getsprfactor,
	.setBW = loraspi_setbandwidth,
	.getBW = loraspi_getbandwidth,
	.getRSSI = loraspi_getrssi,
	.getSNR = loraspi_getsnr,
	.ready2write = loraspi_ready2write,
	.ready2read = loraspi_ready2read,
};

/* The compatible SoC array. */
#ifdef CONFIG_OF
static const struct of_device_id lora_dt_ids[] = {
	{ .compatible = "semtech,sx1276" },
	{ .compatible = "semtech,sx1277" },
	{ .compatible = "semtech,sx1278" },
	{ .compatible = "semtech,sx1279" },
	{ .compatible = "sx1278" },
	{},
};
MODULE_DEVICE_TABLE(of, lora_dt_ids);
#endif

#ifdef CONFIG_ACPI

/* The compatible ACPI device array. */
#define LORA_ACPI_DUMMY	1
static const struct acpi_device_id lora_acpi_ids[] = {
	{ .id = "sx1278" },
	{},
};
MODULE_DEVICE_TABLE(acpi, lora_acpi_ids);

/* The callback function of ACPI probes LoRa SPI. */
static void loraspi_probe_acpi(struct spi_device *spi) {
	const struct acpi_device_id *id;

	if (!has_acpi_companion(&(spi->dev)))
		return;

	id = acpi_match_device(lora_acpi_ids, &(spi->dev));
	if (WARN_ON(!id))
		return;

	if (id->driver_data == LORA_ACPI_DUMMY)
		dev_warn(&(spi->dev),
			"Do not use this driver in produciton systems.\n");
}
#else
static void loraspi_probe_acpi(struct spi_device *spi) {};
#endif

/* The compatible SPI device id array. */
static const struct spi_device_id spi_ids[] = {
	{ .name = "sx1278" },
	{},
};
MODULE_DEVICE_TABLE(spi, spi_ids);

/* The SPI probe callback function. */
static int loraspi_probe(struct spi_device *spi)
{
	struct lora_struct *lrdata;
	struct device *dev;
	unsigned long minor;
	int v;
	int status;

	dev_dbg(&(spi->dev), "probe a LoRa SPI device\n");

#ifdef CONFIG_OF
	if (spi->dev.of_node && !of_match_device(lora_dt_ids, &(spi->dev))) {
		dev_err(&(spi->dev), "buggy DT: LoRa listed directly in DT\n");
		WARN_ON(spi->dev.of_node &&
			!of_match_device(lora_dt_ids, (&spi->dev)));
	}
#endif

	loraspi_probe_acpi(spi);

	/* Initial the SX127X chip. */
	v = init_sx127X(spi);
	if(v < 0) {
		dev_err(&(spi->dev), "no LoRa SPI device, error: %d\n", v);
		return v;
	}
	dev_info(&(spi->dev), "probe a LoRa SPI device with chip ver. %d.%d\n",
			(v >> 4) & 0xF,
			v & 0xF);

	/* Allocate lora device's data. */
	lrdata = kzalloc(sizeof(struct lora_struct), GFP_KERNEL);
	if (!lrdata)
		return -ENOMEM;

	/* Initial the lora device's data. */
	lrdata->lora_device = spi;
	lrdata->ops = &lrops;
	mutex_init(&(lrdata->buf_lock));
	mutex_lock(&minors_lock);
	minor = find_first_zero_bit(minors, N_LORASPI_MINORS);
	if (minor < N_LORASPI_MINORS) {
		set_bit(minor, minors);
		lrdata->devt = MKDEV(lr_driver.major, minor);
		dev = device_create(lr_driver.lora_class,
				&(spi->dev),
				lrdata->devt,
				lrdata,
				"loraSPI%d.%d",
				spi->master->bus_num, spi->chip_select);
		/* Set the SPI device's driver data for later use.  */
		spi_set_drvdata(spi, lrdata);
		lora_device_add(lrdata);
		status = PTR_ERR_OR_ZERO(dev);
	}
	else {
		/* No more lora device available. */
		kfree(lrdata);
		status = -ENODEV;
	}

	mutex_unlock(&minors_lock);

	return status;
}

/* The SPI remove callback function. */
static int loraspi_remove(struct spi_device *spi)
{
	struct lora_struct *lrdata;

	dev_info(&(spi->dev), "remove a LoRa SPI device");

	lrdata = spi_get_drvdata(spi);

	/* Clear the lora device's data. */
	lrdata->lora_device = NULL;
	/* No more operations to the lora device from user space. */
	lora_device_remove(lrdata);
	mutex_lock(&minors_lock);
	device_destroy(lr_driver.lora_class, lrdata->devt);
	clear_bit(MINOR(lrdata->devt), minors);
	/* Set the SX127X chip to sleep. */
	sx127X_setState(spi, SX127X_SLEEP_MODE);
	mutex_unlock(&minors_lock);

	/* Free the memory of the lora device.  */
	kfree(lrdata);

	return 0;
}

/* The SPI driver which acts as a protocol driver in this kernel module. */
static struct spi_driver lora_spi_driver = {
	.driver = {
		.name = __DRIVER_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = lora_dt_ids,
#endif
#ifdef CONFIG_ACPI
		.acpi_match_table = ACPI_PTR(lora_acpi_ids),
#endif
	},
	.probe = loraspi_probe,
	.remove = loraspi_remove,
	.id_table = spi_ids,
};


/* LoRa-SPI kernel module's initial function. */
static int loraspi_sx1278_init(void)
{
	int status;

	pr_debug("sx1278: init SX1278 compatible kernel module\n");

	/* Register a kind of LoRa driver. */
	lora_register_driver(&lr_driver);

	/* Register LoRa SPI driver as an SPI driver. */
	status = spi_register_driver(&lora_spi_driver);

	return status;
}

/* LoRa-SPI kernel module's exit function. */
static void loraspi_sx1278_exit(void)
{
	pr_debug("sx1278: exit\n");

	/* Unregister the LoRa SPI driver. */
	spi_unregister_driver(&lora_spi_driver);
	/* Unregister the lora driver. */
	lora_unregister_driver(&lr_driver);
}

module_init(loraspi_sx1278_init);
module_exit(loraspi_sx1278_exit);

MODULE_AUTHOR("Jian-Hong Pan, <starnight@g.ncu.edu.tw>");
MODULE_DESCRIPTION("LoRa device driver with SPI interface");
MODULE_LICENSE("Dual BSD/GPL");
