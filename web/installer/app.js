// app.js - the installer page: connects to the RPi (Web Serial to a running
// piegpu, or WebUSB to start a blank RPi first: rpiboot.js), shows its
// card, and writes the firmware and the settings onto it (pgi.js); or
// prepares a card in this computer's card reader (card.js).

import {USB_FILTERS, bootStage, waitForBootDevice} from './rpiboot.js';
import {SERIAL_FILTERS, Installer, crc32} from './pgi.js';
import {BOARDS, DEFAULTS, makeCmdline, makeConfig, otherOptions, parseSettings} from './settings.js';
import {GLStream, runDemo} from './gl.js';
import {makeZip, otherEntries, writeFiles} from './card.js';

const $ = id => document.getElementById (id);
const STORE = 'piegpu-installer-settings';
// the installation guide (docs/installation.md), linked from the messages
const GUIDE = 'https://github.com/anight/piegpu/blob/main/docs/installation.md';

let manifest = null;
let firmware = {};			// board -> Map name -> Uint8Array (fetched once)
let installer = null;			// the connection to piegpu
let card = null;			// the last INFO
let cardHost = null;			// host= in the card's cmdline.txt (null: not read)
let busy = false;

// ---- the log --------------------------------------------------------------------

function log (line)
{
	const pre = $('log');
	const atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 4;
	pre.textContent += line + '\n';
	if (atEnd)
	{
		pre.scrollTop = pre.scrollHeight;
	}
}

// a status line; link: [text, a section of the guide] after it
function status (id, text, kind = '', link = null)
{
	const el = $(id);
	el.hidden = !text;
	el.textContent = text || '';
	el.className = 'status' + (kind ? ' ' + kind : '');
	if (text && link)
	{
		const a = document.createElement ('a');
		a.href = `${GUIDE}#${link[1]}`;
		a.target = '_blank';
		a.textContent = link[0];
		el.append (' ', a);
	}
}

// ---- settings -------------------------------------------------------------------

function loadStored ()
{
	try
	{
		return {...DEFAULTS, ...JSON.parse (localStorage.getItem (STORE) || '{}')};
	}
	catch (e)
	{
		return {...DEFAULTS};
	}
}

function readForm ()
{
	const radio = name => document.querySelector (`input[name=${name}]:checked`)?.value;
	return {
		host: radio ('host'), gud: radio ('gud'), output: radio ('output'), panel: radio ('panel'), hdmiMode: radio ('hdmiMode'),
		hdmiPixels: Number ($('hdmiPixels').value),
		cvtWidth: Number ($('cvtWidth').value), cvtHeight: Number ($('cvtHeight').value),
		cvtRate: Number ($('cvtRate').value),
	};
}

function writeForm (s)
{
	for (const name of ['host', 'gud', 'output', 'panel', 'hdmiMode'])
	{
		const input = document.querySelector (`input[name=${name}][value=${s[name]}]`);
		if (input)
		{
			input.checked = true;
		}
	}
	const pixels = $('hdmiPixels');
	if (![...pixels.options].some (o => Number (o.value) === s.hdmiPixels))
	{
		pixels.add (new Option (`at most ${s.hdmiPixels} pixels`, s.hdmiPixels));
	}
	pixels.value = s.hdmiPixels;
	$('cvtWidth').value = s.cvtWidth;
	$('cvtHeight').value = s.cvtHeight;
	$('cvtRate').value = s.cvtRate;
	settingsChanged ();
}

function settingsChanged ()
{
	const s = readForm ();
	const custom = s.hdmiMode === 'custom';
	for (const id of ['cvtWidth', 'cvtHeight', 'cvtRate'])
	{
		$(id).disabled = !custom;
	}
	$('preview-cmdline').textContent = '# cmdline.txt\n' + makeCmdline (s);
	$('preview-config').textContent = '# config.txt\n' + makeConfig (s, currentBoard () ?? 'any');
	try
	{
		localStorage.setItem (STORE, JSON.stringify (s));
	}
	catch (e)
	{
		// (private window)
	}
}

function settingsValid (s)
{
	return s.hdmiMode !== 'custom'
	       || (s.cvtWidth >= 320 && s.cvtWidth <= 1920 && s.cvtHeight >= 200 && s.cvtHeight <= 1200
		   && s.cvtRate >= 24 && s.cvtRate <= 75);
}

// ---- firmware -------------------------------------------------------------------

async function loadManifest ()
{
	try
	{
		manifest = await (await fetch ('firmware/manifest.json', {cache: 'no-cache'})).json ();
		// the version alone (the boards: the notice below; the build times:
		// the card's firmware shows its own)
		const version = document.createElement ('b');
		version.textContent = manifest.version + (manifest.git ? ` (${manifest.git})` : '');
		$('version').replaceChildren ('Current version: ', version);
	}
	catch (e)
	{
		$('version').textContent = 'The firmware files are missing (web/installer/make-firmware.sh).';
	}
}

// 'zero', 'zerow' or 'zero2': the board a running piegpu reports (INFO; an
// older one doesn't say: a Zero); null before one has answered. Only these are
// supported for now (BOARDS): another board counts as a Zero, and the board
// facts say it isn't supported
function currentBoard ()
{
	if (!card)
	{
		return null;
	}
	return /Zero 2/.test (card.board) ? 'zero2' : /Zero W/.test (card.board) ? 'zerow' : 'zero';
}

// the firmware's files for a board (its kernel: kernel.img or kernel8.img)
function firmwareNames (board)
{
	return manifest?.boards?.[board]?.files.map (f => f.name)
	       ?? ['bootcode.bin', 'start.elf', 'fixup.dat', board === 'zero2' ? 'kernel8.img' : 'kernel.img'];
}

async function loadFirmware (board)
{
	if (firmware[board])
	{
		return firmware[board];
	}
	const set = manifest?.boards?.[board];
	if (!set)
	{
		throw new Error (manifest ? `This page has no firmware for the ${BOARDS[board]} (make-firmware.sh)`
					  : 'No firmware on this page (firmware/manifest.json)');
	}
	const files = new Map ();
	for (const f of set.files)
	{
		const path = `firmware/${board}/${f.name}`;
		const bytes = new Uint8Array (await (await fetch (path, {cache: 'no-cache'})).arrayBuffer ());
		const crc = crc32 (bytes).toString (16).padStart (8, '0');
		if (bytes.length !== f.size || crc !== f.crc32)
		{
			throw new Error (`${path} doesn't match the manifest`);
		}
		files.set (f.name, bytes);
	}
	firmware[board] = files;
	return files;
}

// the files to start a blank board over USB: every board's (the Pi firmware
// asks for its own kernel: kernel.img, or kernel8.img on a Zero 2 W with the
// config.txt's [pi02] lines), plus this page's settings. A Zero W starts with
// the Zero's kernel.img (which board it is shows only once it runs); its own
// goes onto the card
async function bootFiles ()
{
	const files = new Map ();
	for (const board of Object.keys (manifest?.boards || {zero: null}).filter (b => b !== 'zerow'))
	{
		for (const [name, bytes] of await loadFirmware (board))
		{
			files.set (name, bytes);
		}
	}
	const s = readForm ();
	files.set ('config.txt', new TextEncoder ().encode (makeConfig (s, 'any')));
	files.set ('cmdline.txt', new TextEncoder ().encode (makeCmdline (s)));
	return files;
}

// ---- connecting -----------------------------------------------------------------

// a connection to piegpu on this port (instead of any other)
async function attach (port, onLine = log)
{
	const old = installer;
	installer = null;
	await old?.close ();
	const connection = new Installer (port, onLine, () =>
	{
		if (installer === connection)		// (not one replaced since)
		{
			installer = null;
			updateButtons ();
			if (!busy)
			{
				status ('connect-status', 'The RPi went away.', 'warn');
			}
		}
	});
	await connection.open ();
	installer = connection;
}

async function openInstaller (port)
{
	await attach (port);
	status ('connect-status', 'Connected to piegpu.', 'ok');
	try
	{
		await refreshCard (true);
	}
	catch (e)
	{
		// no answer: piegpu may be taking GL commands from the serial port
		// (a program ended without restarting it): restart it, ask again
		if (!/^No answer/.test (e.message))
		{
			throw e;
		}
		status ('connect-status', 'piegpu doesn\'t answer (busy with GL commands?): restarting it…', 'warn');
		const connection = installer;
		await restartRPi (() => connection.restart ());
		status ('connect-status', 'Connected to piegpu (restarted).', 'ok');
	}
}

async function connect ()
{
	await run (async () =>
	{
		const known = (await navigator.serial.getPorts ()).filter (p => matchesSerial (p));
		let port = known[0];
		if (!port)
		{
			try
			{
				port = await navigator.serial.requestPort ({filters: SERIAL_FILTERS});
			}
			catch (e)
			{
				if (e.name !== 'NotFoundError')
				{
					throw e;
				}
				status ('connect-status', 'No port picked. If piegpu wasn\'t in the list:', 'warn',
					['what to check', 'troubleshooting']);
				return;
			}
		}
		try
		{
			await openInstaller (port);
		}
		catch (e)
		{
			// the port picked, but the system didn't let the browser open it
			if (e.name === 'NetworkError' && /open/i.test (e.message) && /Linux/.test (navigator.userAgent))
			{
				status ('connect-status', `${e.message} Your user may have no access to the RPi's serial port: `
					+ 'run "sudo usermod -aG dialout $USER" once, then log out and in again.', 'bad',
					['more', 'access-to-usb-devices']);
				return;
			}
			throw e;
		}
	});
}

function matchesSerial (port)
{
	const info = port.getInfo ();
	return SERIAL_FILTERS.some (f => f.usbVendorId === info.usbVendorId && f.usbProductId === info.usbProductId);
}

// a port of piegpu showing up (after starting it, or its reboot)
function waitForSerial (timeoutMs)
{
	return new Promise (resolve =>
	{
		const onConnect = e =>
		{
			if (matchesSerial (e.target))
			{
				finish (e.target);
			}
		};
		const timer = setTimeout (() => finish (null), timeoutMs);
		function finish (port)
		{
			clearTimeout (timer);
			navigator.serial.removeEventListener ('connect', onConnect);
			resolve (port);
		}
		navigator.serial.addEventListener ('connect', onConnect);
	});
}

// the page as rpiboot: the boot ROM, then bootcode.bin's file server, then
// piegpu's serial port. Each new USB device may need the user's pick.
async function bootBlank ()
{
	await run (async () =>
	{
		let device;
		try
		{
			device = await navigator.usb.requestDevice ({filters: USB_FILTERS});	// (the click's)
		}
		catch (e)
		{
			if (e.name !== 'NotFoundError')
			{
				throw e;
			}
			status ('connect-status', 'No device picked. If the RPi wasn\'t in the list:', 'warn',
				['what to check', 'troubleshooting']);
			return;
		}
		const files = await bootFiles ();
		for (let stage = 0; stage < 3; stage++)
		{
			const result = await bootStage (device, files, log);
			if (result === 'done')
			{
				break;
			}
			status ('connect-status', 'bootcode.bin is starting…');
			device = await waitForBootDevice (5000);
			if (!device)
			{
				status ('connect-status', 'The RPi is ready for the next step. Press Continue and pick it again.', 'warn');
				device = await pickAgain (() => navigator.usb.requestDevice ({filters: USB_FILTERS}));
			}
		}
		status ('connect-status', 'piegpu is starting…');
		let port = await waitForSerial (15000);
		if (!port)
		{
			status ('connect-status', 'piegpu is running. Press Continue and pick it.', 'warn');
			port = await pickAgain (() => navigator.serial.requestPort ({filters: SERIAL_FILTERS}));
		}
		await new Promise (r => setTimeout (r, 500));
		await openInstaller (port);
	});
}

// a chooser needs a click: the Continue button's
function pickAgain (pick)
{
	const button = $('continue');
	button.hidden = false;
	return new Promise ((resolve, reject) =>
	{
		button.onclick = async () =>
		{
			try
			{
				resolve (await pick ());
			}
			catch (e)
			{
				reject (e);
			}
			finally
			{
				button.hidden = true;
			}
		};
	});
}

// ---- a card prepared on this computer ---------------------------------------------

// the panel open: the settings (section 3) show too, as they go on the card
function preparing ()
{
	return !$('prepare-panel').hidden;
}

// a card's files for this board: the firmware and the settings
async function cardFiles (board)
{
	const s = readForm ();
	if (!settingsValid (s))
	{
		throw new Error ('The HDMI mode in the settings is out of range.');
	}
	const files = new Map (await loadFirmware (board));
	files.set ('config.txt', new TextEncoder ().encode (makeConfig (s, board)));
	files.set ('cmdline.txt', new TextEncoder ().encode (makeCmdline (s)));
	return files;
}

// the files written into the folder the user picks (the mounted card)
async function prepareWrite ()
{
	const board = $('prepare-board').value;
	if (!('showDirectoryPicker' in window))
	{
		status ('prepare-status', 'This browser can\'t write into a folder: download the zip and unpack it '
			+ 'onto the card.', 'warn', ['how', 'way-a-with-a-card-reader']);
		return;
	}
	await run (async () =>
	{
		status ('prepare-status', 'Choose the card (its top folder)…');
		let dir;
		try
		{
			dir = await window.showDirectoryPicker ({id: 'piegpu-card', mode: 'readwrite'});
		}
		catch (e)
		{
			if (e.name !== 'AbortError')	// (the picker was closed)
			{
				status ('prepare-status', `${e.message} Pick the card itself, or download the zip.`, 'bad',
					['more', 'troubleshooting']);
				return;
			}
			status ('prepare-status', 'Nothing written.');
			return;
		}
		const files = await cardFiles (board);
		const others = await otherEntries (dir, new Set (files.keys ()));
		if (others.length && !confirm (`"${dir.name}" holds ${others.length} other item${others.length > 1 ? 's' : ''} `
					       + `(${others.slice (0, 5).join (', ')}${others.length > 5 ? ', …' : ''}). `
					       + 'Is it the card? Write piegpu\'s files there?'))
		{
			status ('prepare-status', 'Nothing written.');
			return;
		}
		const total = [...files.values ()].reduce ((n, b) => n + b.length, 0);
		const bar = $('prepare-progress');
		bar.hidden = false;
		bar.max = total;
		bar.value = 0;
		status ('prepare-status', `Writing to "${dir.name}"…`);
		try
		{
			await writeFiles (dir, files, n => bar.value = n);
		}
		finally
		{
			bar.hidden = true;
		}
		log (`(page) wrote ${files.size} files, ${total} bytes, to "${dir.name}", each read back and checked`);
		status ('prepare-status', `Done: piegpu ${manifest.version} for the ${BOARDS[board]} on "${dir.name}" `
			+ `(${files.size} files, each read back and checked). Eject the card, put it in the RPi, `
			+ 'connect the RPi\'s USB port and press Connect.', 'ok');
	}, 'prepare-status');
}

// the same files as a zip, to unpack onto the card by hand
async function prepareZip ()
{
	const board = $('prepare-board').value;
	await run (async () =>
	{
		const files = await cardFiles (board);
		const licence = await fetch ('firmware/LICENCE.broadcom');	// (goes along with the RPi's firmware)
		if (licence.ok)
		{
			files.set ('LICENCE.broadcom', new Uint8Array (await licence.arrayBuffer ()));
		}
		const zip = makeZip (files);
		const a = document.createElement ('a');
		a.href = URL.createObjectURL (zip);
		a.download = `piegpu-${manifest.version}-${board}.zip`;
		a.click ();
		setTimeout (() => URL.revokeObjectURL (a.href), 60000);
		status ('prepare-status', `Downloaded ${a.download}: unpack it onto the card (its top folder), eject `
			+ 'the card, put it in the RPi, connect the RPi\'s USB port and press Connect.', 'ok');
	}, 'prepare-status');
}

// ---- the card -------------------------------------------------------------------

function mb (n)
{
	return n >= 1024 ? `${(n / 1024).toFixed (1)} GB` : `${n} MB`;
}

async function refreshCard (loadSettings)
{
	card = await installer.info ();
	settingsChanged ();			// (config.txt for this board)
	const facts = $('card-facts');
	facts.innerHTML = '';
	const add = (k, v) =>
	{
		const dt = document.createElement ('dt');
		const dd = document.createElement ('dd');
		dt.textContent = k;
		dd.textContent = v;
		facts.append (dt, dd);
		return dd;
	};
	$('card-retry').hidden = true;
	$('format').hidden = true;
	add ('Type', card.board ? `${card.board}${card.ramMB ? `, ${card.ramMB} MB` : ''} (revision ${card.revision})`
				  + (/Zero/.test (card.board) ? '' : ': not supported yet (for now: the Zero, the Zero W and the Zero 2 W)')
				: "unknown (this piegpu doesn't say: install to see it)");
	if (!card.card)
	{
		add ('SD card', 'none found');
		status ('card-status', 'Put the microSD card in the RPi now, then press the button.', 'warn');
		$('card-retry').hidden = false;
	}
	else
	{
		const facts = [mb (card.sizeMB), card.fs === 'none' ? 'no file system (blank or unknown)' : card.fs];
		if (card.fs.startsWith ('FAT'))
		{
			facts.push (`${mb (Math.floor (card.freeKB / 1024))} free`);
			add ('SD card', facts.join (' · '));
			const fw = add ('Firmware', describeInstalled ());
			const offered = kernelUpgrade ();
			if (offered)
			{
				const b = document.createElement ('button');
				b.textContent = 'Upgrade';
				b.className = 'upgrade';
				b.title = `Write this page's kernel (${offered.version}, built ${utc (offered.built)}) `
					  + 'to the card, then restart the RPi: the other files and the settings stay';
				b.onclick = upgradeKernel;
				fw.append (' ', b);
			}
			status ('card-status', '');
			if (loadSettings && (card.files.has ('config.txt') || card.files.has ('cmdline.txt')))
			{
				const decode = b => b ? new TextDecoder ().decode (b) : null;
				const config = decode (await installer.read ('config.txt'));
				const cmdline = decode (await installer.read ('cmdline.txt'));
				writeForm (parseSettings (config, cmdline, readForm ()));
				cardHost = parseSettings (null, cmdline, DEFAULTS).host;
			}
		}
		else
		{
			add ('SD card', facts.join (' · '));
			status ('card-status', 'The card has no FAT file system, which the RPi needs to start from it. '
				+ 'Formatting erases everything on it.', 'warn');
			$('format').hidden = false;
		}
	}
	updateButtons ();
}

// a build time as the builds give it ("2026-09-30T00:05:42Z"; before, to the
// minute: "2026-09-30T00:05Z") for reading
function utc (t)
{
	return (t || '').replace ('T', ' ').replace (/Z$/, ' UTC');
}

// a version's numbers (major.minor.patch.build), or null (an older build's
// version was its git commit)
function versionNumbers (v)
{
	return /^\d+\.\d+\.\d+\.\d+$/.test (v || '') ? v.split ('.').map (Number) : null;
}

// the page's kernel for the board, if its version is higher than the card's
// (or the card's has none: an older build), or the card's is another board's
// (a Zero W with the Zero's, without the wireless code): its build line, else
// null
function kernelUpgrade ()
{
	const board = currentBoard ();
	const offered = manifest?.boards?.[board]?.build;
	const ours = versionNumbers (offered?.version);
	if (!ours || !firmwareNames (board).every (n => card.files.has (n)))
	{
		return null;
	}
	const theirs = versionNumbers (card.build?.version);
	if (!theirs)
	{
		return offered;
	}
	const wireless = config => (config || '').split (',').includes ('wireless');
	if (card.build.config && wireless (card.build.config) !== wireless (offered.config))
	{
		return offered;
	}
	for (let i = 0; i < 4; i++)
	{
		if (ours[i] !== theirs[i])
		{
			return ours[i] > theirs[i] ? offered : null;
		}
	}
	return null;
}

// the page's kernel onto the card (the kernel alone: the other firmware
// files and the settings stay), then the RPi restarts from it
async function upgradeKernel ()
{
	await run (async () =>
	{
		const board = currentBoard ();
		const name = manifest.boards[board].kernel;
		const bytes = (await loadFirmware (board)).get (name);
		const bar = $('progress');
		bar.hidden = false;
		bar.max = bytes.length;
		status ('action-status', `Writing ${name}…`);
		await installer.put (name, bytes, n => bar.value = n);
		log (`(page) wrote ${name}, ${bytes.length} bytes, checked by its CRC`);

		status ('action-status', 'Written. Restarting the RPi from the card…');
		const screen = await restartRPi (() => installer.reboot ());
		status ('action-status', `Upgraded: piegpu ${card.build?.version ?? ''} started from the card.`
				       + (screen ? ' ' + screen + '.' : ''), 'ok');
		status ('connect-status', 'Connected to piegpu.', 'ok');
	}, 'action-status');
	$('progress').hidden = true;
}

// piegpu on the card: its build (version, time, configuration), or how much
// of it is there
function describeInstalled ()
{
	const names = firmwareNames (currentBoard ());
	const have = names.filter (n => card.files.has (n));
	if (have.length < names.length)
	{
		return have.length ? `partly on the card (${have.join (', ')})` : 'not on the card';
	}
	const b = card.build;
	if (!b)
	{
		return 'on the card (an older build that doesn\'t say which)';
	}
	const built = utc (b.built);
	const config = b.config.split (',').map (s => s.replace (/_/g, ' ')).join (', ');
	return `${b.version}${b.git ? ` (${b.git})` : ''}, built ${built} (${config})`;
}

async function format ()
{
	if (!confirm (`Erase everything on the ${mb (card.sizeMB)} card in the RPi and make it one FAT32 partition?`))
	{
		return;
	}
	await run (async () =>
	{
		status ('card-status', 'Formatting…');
		const reply = await installer.command ('PGI FORMAT', 180000);
		if (reply !== 'OK')
		{
			throw new Error (`Formatting failed: ${reply}`);
		}
		await refreshCard (false);
	});
}

// ---- installing -----------------------------------------------------------------

async function install (settingsOnly)
{
	const s = readForm ();
	if (!settingsValid (s))
	{
		status ('action-status', 'The HDMI mode is out of range.', 'bad');
		return;
	}
	await run (async () =>
	{
		const board = currentBoard ();
		const files = settingsOnly ? new Map () : await loadFirmware (board);
		// the card's options this page doesn't set (cpu=, touch=...: by hand, or
		// Settings on the panel) stay
		const old = card?.files?.has ('cmdline.txt') ? await installer.read ('cmdline.txt') : null;
		const other = otherOptions (old ? new TextDecoder ().decode (old) : '');
		const all = [...files.entries (),
			     ['config.txt', new TextEncoder ().encode (makeConfig (s, board))],
			     ['cmdline.txt', new TextEncoder ().encode (makeCmdline (s, other))]];
		const total = all.reduce ((n, [, b]) => n + b.length, 0);
		const bar = $('progress');
		bar.hidden = false;
		bar.max = total;
		let done = 0;
		for (const [name, bytes] of all)
		{
			status ('action-status', `Writing ${name}…`);
			await installer.put (name, bytes, n => bar.value = done + n);
			done += bytes.length;
			bar.value = done;
		}
		log (`(page) wrote ${all.length} files, ${total} bytes, each checked by its CRC`);
		cardHost = s.host;

		status ('action-status', 'Written. Restarting the RPi from the card…');
		const screen = await restartRPi (() => installer.reboot ());
		status ('action-status', `Done: piegpu started from the card.${screen ? ' ' + screen + '.' : ''}`, 'ok');
		status ('connect-status', 'Connected to piegpu.', 'ok');
	}, 'action-status');
	$('progress').hidden = true;
}

// restart the RPi (how: a function) and connect again; returns its
// "Screen: ..." line
async function restartRPi (how)
{
	const waiting = waitForSerial (30000);
	await how ();
	const port = await waiting;
	if (!port)
	{
		throw new Error ('The RPi didn\'t come back from its card within 30 s');
	}
	await new Promise (r => setTimeout (r, 500));
	const seen = [];
	await attach (port, line =>
	{
		log (line);
		if (/ Screen: /.test (line))
		{
			seen.push (line.replace (/^\S+ \S+ /, ''));
		}
	});
	// everything from the board again: its type, the card, the firmware, the
	// settings (its answer comes after the replayed log)
	await refreshCard (true);
	return seen.pop ();
}

// ---- demos on the RPi -----------------------------------------------------------

// the files the Test Video and Test Audio buttons play (web/installer/
// make-firmware.sh puts them here): the Big Buck Bunny trailer (Blender
// Foundation, CC BY 3.0), "Monkeys Spinning Monkeys" (Kevin MacLeod,
// incompetech.com, CC BY 4.0) as an MP3 and as an Ogg Vorbis file (the
// format beside the button)
const TEST_VIDEO = 'media/bbb_trailer-480p.mov';
const TEST_AUDIO = {mp3: 'media/monkeys_spinning_monkeys.mp3', ogg: 'media/monkeys_spinning_monkeys.ogg'};

let demo = null;			// the running demo's GLStream

async function testOpenGL ()
{
	await runOnRPi ('gears', 'The OpenGL test', async () => ({}));
}

async function testVideo ()
{
	await runMedia (TEST_VIDEO, 'video', 'The video');
}

async function testAudio ()
{
	await runMedia (TEST_AUDIO[$('test-audio-format').value], 'sound', 'The sound');
}

// the media demo (demos/media.c) playing url, an MP4, an Ogg or an MP3 (what: its
// name in the messages)
async function runMedia (url, what, title)
{
	await runOnRPi ('media', title, async () =>
	{
		status ('action-status', `Fetching the test ${what}…`);
		const response = await fetch (url);
		if (!response.ok)
		{
			throw new Error (`No test ${what} on this page (${url}: web/installer/make-firmware.sh)`);
		}
		return {'/media': new Uint8Array (await response.arrayBuffer ())};
	});
}

// run demos/NAME.js (WebAssembly) on the RPi over the serial port; files ():
// its files. Then the RPi restarts: it keeps taking commands from the
// serial port until it does.
async function runOnRPi (name, title, files)
{
	if (cardHost === 'i2s')
	{
		status ('action-status', 'The card has Host: I2S, so the RPi ignores commands from this page. '
			+ 'Choose Auto or USB above and save the settings first.', 'warn');
		return;
	}
	await run (async () =>
	{
		const connection = installer;
		const demoFiles = await files ();
		demo = new GLStream (bytes => connection.writeRaw (bytes));
		connection.rawSink = chunk => demo.push (chunk);
		$('stop-demo').hidden = false;
		$('volume-control').hidden = name !== 'media';
		status ('action-status', 'Starting…');
		let code;
		try
		{
			code = await runDemo (name, demo, line =>
			{
				if (line.trim ())
				{
					log (`(${name}) ${line}`);
				}
				const v = line.match (/^media: volume (\d+)%/);
				if (v)
				{
					$('volume').value = v[1];		// (the RPi's, or the last set)
					$('volume-value').textContent = `${v[1]}%`;
				}
				const m = line.match (new RegExp (`^${name}: (.*fps.*)$`));
				const sound = /^media: sound: /.test (line) && !/^Running/.test ($('action-status').textContent);
				if (m || sound || / running$/.test (line))
				{
					status ('action-status', m ? `Running: ${m[1]}.` : 'Running.', 'ok');
				}
			}, demoFiles);
		}
		finally
		{
			connection.rawSink = null;
			demo = null;
			$('stop-demo').hidden = true;
			$('volume-control').hidden = true;
		}

		// the session ends (STREAM_END): the RPi resets and answers this page
		// again; if it doesn't (older firmware), it restarts
		status ('action-status', code ? `${title} ended with an error (see the log). Ending the session…`
				       : 'Stopped. Ending the session…', code ? 'bad' : '');
		if (await connection.endStream ())
		{
			status ('action-status', code ? `${title} ended with an error (see the log).` : 'Stopped.',
				code ? 'bad' : 'ok');
		}
		else
		{
			status ('action-status', 'The RPi didn\'t end the session: restarting it…', 'warn');
			await restartRPi (() => connection.restart ());
			status ('action-status', code ? `${title} ended with an error (see the log); the RPi restarted.`
					       : 'Stopped; the RPi restarted.', code ? 'bad' : 'ok');
			status ('connect-status', 'Connected to piegpu.', 'ok');
		}
	}, 'action-status');
}

// ---- reset ----------------------------------------------------------------------

// restart the RPi from its card (the reboot magic: in any mode, a running
// GL stream too) and connect again
async function reset ()
{
	await run (async () =>
	{
		status ('connect-status', 'Restarting the RPi from its card…');
		const connection = installer;
		const screen = await restartRPi (() => connection.restart ());
		status ('connect-status', `The RPi restarted; connected.${screen ? ' ' + screen + '.' : ''}`, 'ok');
	});
}

// ---- plumbing -------------------------------------------------------------------

async function run (task, statusId = 'connect-status')
{
	if (busy)
	{
		return;
	}
	busy = true;
	updateButtons ();
	try
	{
		await task ();
	}
	catch (e)
	{
		if (e.name !== 'NotFoundError')		// (the chooser was closed)
		{
			status (statusId, e.message, 'bad');
			log (`(page) ${e.message}`);
		}
	}
	finally
	{
		busy = false;
		$('continue').hidden = true;
		updateButtons ();
	}
}

function updateButtons ()
{
	const fat = !!(installer && card?.card && card.fs.startsWith ('FAT'));
	$('install').disabled = busy || !fat;
	$('save-settings').disabled = busy || !fat || !firmwareNames (currentBoard ()).every (n => card.files.has (n));
	$('connect').disabled = busy;
	$('boot').disabled = busy;
	$('format').disabled = busy;
	$('card-retry').disabled = busy;
	$('run-gears').disabled = busy || !installer;
	$('test-video').disabled = busy || !installer;
	$('test-audio').disabled = busy || !installer;
	$('test-audio-format').disabled = busy || !installer;
	$('reset').disabled = busy || !installer;
	$('prepare').disabled = busy;
	$('prepare-write').disabled = busy || !$('prepare-board').value;
	$('prepare-zip').disabled = busy || !$('prepare-board').value;

	// the board, its settings and the actions: while a board is connected
	// (kept while an action restarts it); the settings while preparing a card
	if (!busy)
	{
		for (const id of ['card-section', 'settings-section', 'actions-section'])
		{
			$(id).hidden = !(installer && card) && !(id === 'settings-section' && preparing ());
		}
	}
}

// section 3 shown or hidden (its button; hidden until shown, remembered in
// this browser)
const SETTINGS_OPEN = 'piegpu-settings-open';

function showSettings (open)
{
	$('settings-body').hidden = !open;
	$('settings-toggle').textContent = open ? 'Hide' : 'Show';
	$('settings-toggle').setAttribute ('aria-expanded', String (open));
	try
	{
		localStorage.setItem (SETTINGS_OPEN, open ? '1' : '0');
	}
	catch (e)
	{
	}
}

function init ()
{
	if (!('usb' in navigator) || !('serial' in navigator))
	{
		$('unsupported').hidden = false;
		for (const id of ['connect', 'boot'])
		{
			$(id).disabled = true;
		}
	}
	writeForm (loadStored ());
	for (const el of document.querySelectorAll ('input, select'))
	{
		el.addEventListener ('input', settingsChanged);
	}
	$('connect').onclick = connect;
	$('boot').onclick = bootBlank;
	$('prepare').onclick = () =>
	{
		$('prepare-panel').hidden = !$('prepare-panel').hidden;
		updateButtons ();
	};
	$('prepare-board').onchange = updateButtons;
	$('prepare-write').onclick = prepareWrite;
	$('prepare-zip').onclick = prepareZip;
	$('format').onclick = format;
	$('card-retry').onclick = () => run (() => refreshCard (true));
	$('install').onclick = () => install (false);
	$('save-settings').onclick = () => install (true);
	$('run-gears').onclick = testOpenGL;
	$('test-video').onclick = testVideo;
	$('test-audio').onclick = testAudio;
	$('stop-demo').onclick = () => demo?.stop ();
	$('volume').oninput = () =>
	{
		$('volume-value').textContent = `${$('volume').value}%`;
		if (demo)
		{
			demo.volumeRequest = Number ($('volume').value);	// (demos/media.c takes it)
		}
	};
	$('reset').onclick = reset;
	$('settings-toggle').onclick = () => showSettings ($('settings-body').hidden);
	let open = false;
	try
	{
		open = localStorage.getItem (SETTINGS_OPEN) === '1';
	}
	catch (e)
	{
	}
	showSettings (open);
	loadManifest ();
	updateButtons ();
	if ('usb' in navigator && 'serial' in navigator)
	{
		noteKnownPort ();
	}
}

// a piegpu this page may use already (the user picked it before)
async function noteKnownPort ()
{
	const known = (await navigator.serial.getPorts ()).filter (p => matchesSerial (p));
	if (known.length)
	{
		log ('(page) a piegpu the page knows is connected: press Connect');
	}
}

init ();
