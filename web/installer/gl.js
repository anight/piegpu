// gl.js - a demo compiled to WebAssembly (hosts/web: gears) driving the RPi
// over the page's serial port. The demo's transport (transports/pc-usb/
// pgpu_host.c, built with Emscripten) calls Module.glIO: a GLStream, which
// holds what the port has received and writes to the port. The demo runs
// until stop (): its next read, write or wait then ends it.

export class GLStream
{
	// write (bytes): a promise, done when the port has taken them
	constructor (write)
	{
		this.writeBytes = write;
		this.chunks = [];
		this.offset = 0;			// into chunks[0]
		this.waiter = null;
		this.stopped = false;
		this.volumeRequest = -1;		// a new volume for the demo (percent), or -1
	}

	// bytes from the port
	push (chunk)
	{
		this.chunks.push (chunk);
		this.wake ();
	}

	stop ()
	{
		this.stopped = true;
		this.wake ();
	}

	wake ()
	{
		const waiter = this.waiter;
		this.waiter = null;
		waiter?.();
	}

	// ---- for the demo (< 0: stopped) ----

	async write (bytes)
	{
		if (this.stopped)
		{
			return -1;
		}
		await this.writeBytes (bytes);
		return this.stopped ? -1 : 0;
	}

	read (into)
	{
		if (this.stopped)
		{
			return -1;
		}
		let n = 0;
		while (n < into.length && this.chunks.length)
		{
			const chunk = this.chunks[0];
			const k = Math.min (chunk.length - this.offset, into.length - n);
			into.set (chunk.subarray (this.offset, this.offset + k), n);
			n += k;
			this.offset += k;
			if (this.offset === chunk.length)
			{
				this.chunks.shift ();
				this.offset = 0;
			}
		}
		return n;
	}

	// until bytes come, or us microseconds
	wait (us)
	{
		if (this.stopped || this.chunks.length)
		{
			return Promise.resolve (this.stopped ? -1 : 0);
		}
		return new Promise (resolve =>
		{
			const done = () =>
			{
				clearTimeout (timer);
				this.waiter = null;
				resolve (this.stopped ? -1 : 0);
			};
			const timer = setTimeout (done, Math.max (1, us / 1000));
			this.waiter = done;
		});
	}
}

// run demos/NAME.js with this stream; print (line): its output; files: path
// -> bytes, put in its file system first (the media demo's /media).
// Resolves with its exit code when it ends (stopped: 0).
export async function runDemo (name, stream, print, files = {})
{
	const createDemo = (await import (new URL (`./demos/${name}.js`, import.meta.url))).default;
	return new Promise ((resolve, reject) =>
	{
		createDemo ({
			glIO: stream,
			print,
			printErr: print,
			preRun: [module => Object.entries (files).forEach (([path, bytes]) => module.FS.writeFile (path, bytes))],
			onExit: resolve,
			onAbort: what => reject (new Error (`${name}: ${what}`)),
		}).catch (reject);
	});
}
