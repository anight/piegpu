// card.js - a card prepared on this computer (instead of by the RPi): piegpu's
// files written into the card's folder (the File System Access API: the
// folder the user picks, the mounted card), each read back and checked, or
// put in a zip to unpack onto the card by hand.

import {crc32} from './pgi.js';

// the names a card's folder may hold besides piegpu's, which don't make it
// look like some other folder (what systems put on a new or used card)
const CARD_LITTER = new Set (['System Volume Information', 'LOST.DIR', '.Trashes', '.Trash-1000',
			      '.fseventsd', '.Spotlight-V100', '.TemporaryItems', 'desktop.ini']);

// the entries in dir other than names (and CARD_LITTER): a folder that has
// many isn't a new card
export async function otherEntries (dir, names)
{
	const others = [];
	for await (const [name] of dir.entries ())
	{
		if (!names.has (name) && !CARD_LITTER.has (name))
		{
			others.push (name);
		}
	}
	return others;
}

// files (name -> bytes) into dir, each read back and compared by its CRC;
// progress (bytes written so far)
export async function writeFiles (dir, files, progress = () => {})
{
	let done = 0;
	for (const [name, bytes] of files)
	{
		const handle = await dir.getFileHandle (name, {create: true});
		const out = await handle.createWritable ();	// (from empty: keepExistingData is false)
		await out.write (bytes);
		await out.close ();
		const back = new Uint8Array (await (await handle.getFile ()).arrayBuffer ());
		if (back.length !== bytes.length || crc32 (back) !== crc32 (bytes))
		{
			throw new Error (`${name} reads back differently from what was written`);
		}
		done += bytes.length;
		progress (done);
	}
}

// files (name -> bytes) as a zip without compression (PKWARE APPNOTE 4.3:
// local headers, the central directory, its end)
export function makeZip (files)
{
	const encoder = new TextEncoder ();
	const parts = [], central = [];
	let offset = 0;
	const le = (n, bytes) => Array.from ({length: bytes}, (_, i) => (n >>> (8 * i)) & 0xff);
	for (const [name, bytes] of files)
	{
		const path = encoder.encode (name);
		const crc = crc32 (bytes);
		// version 2.0, no flags, stored, time 00:00 of 1980-01-01, crc, sizes, name
		const common = [...le (20, 2), ...le (0, 2), ...le (0, 2), ...le (0, 2), ...le (0x21, 2),
				...le (crc, 4), ...le (bytes.length, 4), ...le (bytes.length, 4), ...le (path.length, 2)];
		const local = new Uint8Array ([...le (0x04034b50, 4), ...common, ...le (0, 2)]);
		parts.push (local, path, bytes);
		central.push (new Uint8Array ([...le (0x02014b50, 4), ...le (20, 2), ...common,
					       ...le (0, 2), ...le (0, 2), ...le (0, 2), ...le (0, 2), ...le (0, 4),
					       ...le (offset, 4)]), path);
		offset += local.length + path.length + bytes.length;
	}
	const centralBytes = central.reduce ((n, p) => n + p.length, 0);
	const end = new Uint8Array ([...le (0x06054b50, 4), ...le (0, 2), ...le (0, 2), ...le (files.size, 2),
				     ...le (files.size, 2), ...le (centralBytes, 4), ...le (offset, 4), ...le (0, 2)]);
	return new Blob ([...parts, ...central, end], {type: 'application/zip'});
}
