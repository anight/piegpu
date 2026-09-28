// rpiboot.js - boots a Raspberry Pi Zero over its USB port from the page
// (WebUSB), as raspberrypi/usbboot's rpiboot does: a Zero without a bootable
// SD card waits in its boot ROM (USB 0a5c:2763) for bootcode.bin; that then
// asks for the rest (config.txt, start.elf, fixup.dat, kernel.img, ...) file
// by file, and the page serves them from memory.
//
// The protocol (usbboot main.c): a "write" is a vendor control transfer
// carrying the length (wValue low 16 bits, wIndex high), then the bytes on
// the bulk OUT endpoint; a "read" is a vendor control IN transfer of that
// length. The ROM stage has no serial number string (iSerialNumber 0), the
// file server stage (bootcode.bin running) has one.

export const USB_FILTERS = [
	{vendorId: 0x0a5c, productId: 0x2763},	// BCM2835/6/7 boot ROM, and bootcode.bin
	{vendorId: 0x0a5c, productId: 0x2764},
];

const CHUNK = 16384;

class BootDevice
{
	constructor (device)
	{
		this.device = device;
	}

	async open ()
	{
		const d = this.device;
		await d.open ();
		if (!d.configuration)
		{
			await d.selectConfiguration (1);
		}
		// one interface: the vendor one; two (BCM2837): mass storage first
		const two = d.configuration.interfaces.length > 1;
		this.iface = two ? 1 : 0;
		this.outEp = two ? 3 : 1;
		await d.claimInterface (this.iface);
	}

	async close ()
	{
		try
		{
			await this.device.close ();
		}
		catch (e)
		{
			// gone already (it re-enumerates)
		}
	}

	async write (data)
	{
		const n = data ? data.length : 0;
		const r = await this.device.controlTransferOut (
			{requestType: 'vendor', recipient: 'device', request: 0, value: n & 0xffff, index: n >>> 16});
		if (r.status !== 'ok')
		{
			throw new Error (`control transfer: ${r.status}`);
		}
		for (let at = 0; at < n; at += CHUNK)
		{
			const part = data.subarray (at, Math.min (n, at + CHUNK));
			const w = await this.device.transferOut (this.outEp, part);
			if (w.status !== 'ok' || w.bytesWritten !== part.length)
			{
				throw new Error (`bulk transfer: ${w.status}, ${w.bytesWritten} of ${part.length} bytes`);
			}
		}
	}

	// the size alone (GetFileSize's answer): a control transfer, no data
	async announce (size)
	{
		await this.device.controlTransferOut (
			{requestType: 'vendor', recipient: 'device', request: 0, value: size & 0xffff, index: size >>> 16});
	}

	async read (n)
	{
		const r = await this.device.controlTransferIn (
			{requestType: 'vendor', recipient: 'device', request: 0, value: n & 0xffff, index: n >>> 16}, n);
		if (r.status !== 'ok')
		{
			throw new Error (`control transfer in: ${r.status}`);
		}
		return new Uint8Array (r.data.buffer, r.data.byteOffset, r.data.byteLength);
	}
}

// the ROM stage: bootcode.bin, after a 24-byte message (length, 20 bytes of
// signature: zeros, unsigned)
async function sendBootcode (dev, bootcode, log)
{
	const message = new Uint8Array (24);
	new DataView (message.buffer).setInt32 (0, bootcode.length, true);
	await dev.write (message);
	await dev.write (bootcode);
	log (`bootcode.bin sent (${bootcode.length} bytes)`);
	await new Promise (r => setTimeout (r, 1000));
	try
	{
		const code = new DataView ((await dev.read (4)).buffer).getInt32 (0, true);
		// (rpiboot reports a nonzero code as a failure, yet the Zero's ROM
		// answered 0x400c0 here and ran bootcode.bin: the next stage tells)
		log (`The boot ROM took it (status 0x${(code >>> 0).toString (16)})`);
	}
	catch (e)
	{
		log ('The boot ROM has gone to run bootcode.bin');	// it may not answer
	}
}

// the file server stage: bootcode.bin and start.elf ask, file by file
async function serveFiles (dev, files, log)
{
	for (;;)
	{
		const m = await dev.read (260);			// int command, char name[256]
		const command = new DataView (m.buffer, m.byteOffset).getInt32 (0, true);
		const end = m.indexOf (0, 4);
		const name = new TextDecoder ().decode (m.subarray (4, end < 0 ? 260 : end));
		if (!name)
		{
			await dev.write (null);
			log ('Boot files done');
			return;
		}
		const file = files.get (name);
		if (command === 0)				// GetFileSize
		{
			if (file)
			{
				await dev.announce (file.length);
			}
			else
			{
				await dev.write (null);
				log (`(no ${name})`);
			}
		}
		else if (command === 1)				// ReadFile
		{
			await dev.write (file || null);
			if (file)
			{
				log (`${name} sent (${file.length} bytes)`);
			}
		}
		else						// Done
		{
			log ('Boot files done');
			return;
		}
	}
}

// Serves one stage of the device the user picked (or that's back); returns
// 'rom' after bootcode.bin (the device re-enumerates for the next stage) or
// 'done' when the files are served (it goes on to boot the kernel).
export async function bootStage (usbDevice, files, log)
{
	const dev = new BootDevice (usbDevice);
	await dev.open ();
	try
	{
		if (!usbDevice.serialNumber)
		{
			log ('The Zero\'s boot ROM is waiting: sending bootcode.bin');
			await sendBootcode (dev, files.get ('bootcode.bin'), log);
			return 'rom';
		}
		log ('bootcode.bin is running: serving the boot files');
		await serveFiles (dev, files, log);
		return 'done';
	}
	finally
	{
		await dev.close ();
	}
}

// the next boot device that shows up (one the page has permission for), or
// null after the timeout
export function waitForBootDevice (timeoutMs)
{
	return new Promise (resolve =>
	{
		const onConnect = e =>
		{
			if (USB_FILTERS.some (f => f.vendorId === e.device.vendorId && f.productId === e.device.productId))
			{
				finish (e.device);
			}
		};
		const timer = setTimeout (() => finish (null), timeoutMs);
		function finish (device)
		{
			clearTimeout (timer);
			navigator.usb.removeEventListener ('connect', onConnect);
			resolve (device);
		}
		navigator.usb.addEventListener ('connect', onConnect);
	});
}
