// pgi.js - the pico-gpu installer protocol (gpu/install/installer.h) over the
// Zero's USB serial port (Web Serial): text lines, the Zero's log between its
// answers (#PGI ...). File data goes as base64 lines of 4.5 KB, each answered
// (the Zero's text path has no flow control).

export const SERIAL_FILTERS = [{usbVendorId: 0x1209, usbProductId: 0x0001}];	// devtools/devlink.h

const DATA_CHUNK = 4608;			// a 6 KB line: the Zero's USB serial queue is 8 KB

let crcTable = null;

export function crc32 (bytes)
{
	if (!crcTable)
	{
		crcTable = new Uint32Array (256);
		for (let i = 0; i < 256; i++)
		{
			let c = i;
			for (let k = 0; k < 8; k++)
			{
				c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
			}
			crcTable[i] = c >>> 0;
		}
	}
	let crc = 0xffffffff;
	for (let i = 0; i < bytes.length; i++)
	{
		crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
	}
	return (crc ^ 0xffffffff) >>> 0;
}

function toBase64 (bytes)
{
	let s = '';
	for (let i = 0; i < bytes.length; i += 4096)
	{
		s += String.fromCharCode (...bytes.subarray (i, i + 4096));
	}
	return btoa (s);
}

function fromBase64 (text)
{
	const s = atob (text);
	const bytes = new Uint8Array (s.length);
	for (let i = 0; i < s.length; i++)
	{
		bytes[i] = s.charCodeAt (i);
	}
	return bytes;
}

export class Installer
{
	// onLog (line): the Zero's log; onClose (): the port went away
	constructor (port, onLog, onClose)
	{
		this.port = port;
		this.onLog = onLog;
		this.onClose = onClose;
		this.waiting = null;			// {resolve, reject, timer}
		this.replies = [];
	}

	async open ()
	{
		await this.port.open ({baudRate: 115200});
		this.writer = this.port.writable.getWriter ();
		this.reading = this.readLoop ();
	}

	async close ()
	{
		try
		{
			this.closing = true;
			await this.reader?.cancel ();
			await this.reading;
			this.writer?.releaseLock ();
			await this.port.close ();
		}
		catch (e)
		{
			// gone already
		}
	}

	async readLoop ()
	{
		const decoder = new TextDecoder ();
		let text = '';
		try
		{
			this.reader = this.port.readable.getReader ();
			for (;;)
			{
				const {value, done} = await this.reader.read ();
				if (done)
				{
					break;
				}
				text += decoder.decode (value, {stream: true});
				let nl;
				while ((nl = text.indexOf ('\n')) >= 0)
				{
					const line = text.slice (0, nl).replace (/\r$/, '');
					text = text.slice (nl + 1);
					if (line.startsWith ('#PGI '))
					{
						this.deliver (line.slice (5));
					}
					else if (line)
					{
						this.onLog?.(line);
					}
				}
			}
		}
		catch (e)
		{
			// the device went away (a reboot)
		}
		finally
		{
			this.reader?.releaseLock ();
			this.fail (new Error ('The Zero went away'));
			if (!this.closing)
			{
				this.onClose?.();
			}
		}
	}

	deliver (reply)
	{
		const w = this.waiting;
		if (w)
		{
			this.waiting = null;
			clearTimeout (w.timer);
			w.resolve (reply);
		}
	}

	fail (error)
	{
		const w = this.waiting;
		if (w)
		{
			this.waiting = null;
			clearTimeout (w.timer);
			w.reject (error);
		}
	}

	// a command line; its answer (without "#PGI ")
	async command (line, timeoutMs = 15000)
	{
		const reply = new Promise ((resolve, reject) =>
		{
			const timer = setTimeout (() =>
			{
				this.waiting = null;
				reject (new Error (`No answer to ${line.split (' ').slice (0, 2).join (' ')}`));
			}, timeoutMs);
			this.waiting = {resolve, reject, timer};
		});
		await this.writer.write (new TextEncoder ().encode (line + '\n'));
		return reply;
	}

	// {card, sizeMB, fs, freeKB, files: Map name -> bytes, error}
	async info ()
	{
		const reply = await this.command ('PGI INFO', 30000);	// the card's first use: its setup
		if (!reply.startsWith ('INFO '))
		{
			throw new Error (reply);
		}
		const f = Object.fromEntries (reply.slice (5).split (' ').map (kv => kv.split ('=')));
		const files = new Map ();
		for (const entry of (f.files && f.files !== '-' ? f.files.split (',') : []))
		{
			const [name, size] = entry.split (':');
			files.set (name, Number (size));
		}
		return {card: f.card === '1', sizeMB: Number (f.size || 0), fs: f.fs || 'none',
			freeKB: Number (f.free || 0), files, error: f.error || null};
	}

	// a small file from the card (null if there's none)
	async read (name)
	{
		const reply = await this.command (`PGI READ ${name}`);
		const m = reply.match (/^FILE (\S+) ?(.*)$/);
		if (!m)
		{
			throw new Error (reply);
		}
		return m[2] === '-' ? null : fromBase64 (m[2]);
	}

	// a file onto the card; onProgress (bytes written)
	async put (name, bytes, onProgress)
	{
		const crc = crc32 (bytes).toString (16).padStart (8, '0');
		let reply = await this.command (`PGI PUT ${name} ${bytes.length} ${crc}`);
		if (reply !== 'OK')
		{
			throw new Error (`${name}: ${reply}`);
		}
		for (let at = 0; at < bytes.length; at += DATA_CHUNK)
		{
			reply = await this.command ('PGI DATA ' + toBase64 (bytes.subarray (at, at + DATA_CHUNK)));
			if (!reply.startsWith ('OK'))
			{
				throw new Error (`${name}: ${reply}`);
			}
			onProgress?.(Math.min (bytes.length, at + DATA_CHUNK));
		}
		reply = await this.command ('PGI END');
		if (reply !== `OK ${name}`)
		{
			throw new Error (`${name}: ${reply}`);
		}
	}

	async reboot ()
	{
		return this.command ('PGI REBOOT', 5000).catch (() => 'OK');	// it may go first
	}
}
