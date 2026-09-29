// app.js - the installer page: connects to the Zero (Web Serial to a running
// pico-gpu, or WebUSB to start a blank Zero first: rpiboot.js), shows its
// card, and writes the firmware and the settings onto it (pgi.js).

import {USB_FILTERS, bootStage, waitForBootDevice} from './rpiboot.js';
import {SERIAL_FILTERS, Installer, crc32} from './pgi.js';
import {DEFAULTS, makeCmdline, makeConfig, parseSettings} from './settings.js';
import {GLStream, runDemo} from './gl.js';

const $ = id => document.getElementById (id);
const FIRMWARE = ['bootcode.bin', 'start.elf', 'fixup.dat', 'kernel.img'];
const STORE = 'pico-gpu-installer-settings';

let manifest = null;
let firmware = null;			// Map name -> Uint8Array (fetched once)
let installer = null;			// the connection to pico-gpu
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

function status (id, text, kind = '')
{
	const el = $(id);
	el.hidden = !text;
	el.textContent = text || '';
	el.className = 'status' + (kind ? ' ' + kind : '');
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
	$('preview-config').textContent = '# config.txt\n' + makeConfig (s);
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
		$('version').textContent = `Firmware ${manifest.version}, built ${manifest.built}.`;
	}
	catch (e)
	{
		$('version').textContent = 'The firmware files are missing (web/installer/make-firmware.sh).';
	}
}

async function loadFirmware ()
{
	if (firmware)
	{
		return firmware;
	}
	if (!manifest)
	{
		throw new Error ('No firmware on this page (firmware/manifest.json)');
	}
	const files = new Map ();
	for (const f of manifest.files)
	{
		const bytes = new Uint8Array (await (await fetch (`firmware/${f.name}`, {cache: 'no-cache'})).arrayBuffer ());
		const crc = crc32 (bytes).toString (16).padStart (8, '0');
		if (bytes.length !== f.size || crc !== f.crc32)
		{
			throw new Error (`firmware/${f.name} doesn't match the manifest`);
		}
		files.set (f.name, bytes);
	}
	firmware = files;
	return files;
}

// the boot files plus this page's settings
async function bootFiles ()
{
	const files = new Map (await loadFirmware ());
	const s = readForm ();
	files.set ('config.txt', new TextEncoder ().encode (makeConfig (s)));
	files.set ('cmdline.txt', new TextEncoder ().encode (makeCmdline (s)));
	return files;
}

// ---- connecting -----------------------------------------------------------------

// a connection to pico-gpu on this port (instead of any other)
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
				status ('connect-status', 'The Zero went away.', 'warn');
			}
		}
	});
	await connection.open ();
	installer = connection;
}

async function openInstaller (port)
{
	await attach (port);
	status ('connect-status', 'Connected to pico-gpu.', 'ok');
	await refreshCard (true);
}

async function connect ()
{
	await run (async () =>
	{
		const known = (await navigator.serial.getPorts ()).filter (p => matchesSerial (p));
		const port = known[0] || await navigator.serial.requestPort ({filters: SERIAL_FILTERS});
		await openInstaller (port);
	});
}

function matchesSerial (port)
{
	const info = port.getInfo ();
	return SERIAL_FILTERS.some (f => f.usbVendorId === info.usbVendorId && f.usbProductId === info.usbProductId);
}

// a port of pico-gpu showing up (after starting it, or its reboot)
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
// pico-gpu's serial port. Each new USB device may need the user's pick.
async function bootBlank ()
{
	await run (async () =>
	{
		let device = await navigator.usb.requestDevice ({filters: USB_FILTERS});	// (the click's)
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
				status ('connect-status', 'The Zero is ready for the next step. Press Continue and pick it again.', 'warn');
				device = await pickAgain (() => navigator.usb.requestDevice ({filters: USB_FILTERS}));
			}
		}
		status ('connect-status', 'pico-gpu is starting…');
		let port = await waitForSerial (15000);
		if (!port)
		{
			status ('connect-status', 'pico-gpu is running. Press Continue and pick it.', 'warn');
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

// ---- the card -------------------------------------------------------------------

function mb (n)
{
	return n >= 1024 ? `${(n / 1024).toFixed (1)} GB` : `${n} MB`;
}

async function refreshCard (loadSettings)
{
	card = await installer.info ();
	const facts = $('card-facts');
	facts.innerHTML = '';
	const add = (k, v) =>
	{
		const dt = document.createElement ('dt');
		const dd = document.createElement ('dd');
		dt.textContent = k;
		dd.textContent = v;
		facts.append (dt, dd);
	};
	$('card-retry').hidden = true;
	$('format').hidden = true;
	add ('Type', card.board ? `${card.board}${card.ramMB ? `, ${card.ramMB} MB` : ''} (revision ${card.revision})`
				: "unknown (this pico-gpu doesn't say: install to see it)");
	if (!card.card)
	{
		add ('SD card', 'none found');
		status ('card-status', 'Put the microSD card in the Zero now, then press the button.', 'warn');
		$('card-retry').hidden = false;
	}
	else
	{
		const facts = [mb (card.sizeMB), card.fs === 'none' ? 'no file system (blank or unknown)' : card.fs];
		if (card.fs.startsWith ('FAT'))
		{
			facts.push (`${mb (Math.floor (card.freeKB / 1024))} free`);
			add ('SD card', facts.join (' · '));
			add ('Firmware', describeInstalled ());
			status ('card-status', '');
			if (loadSettings && (card.files.has ('config.txt') || card.files.has ('cmdline.txt')))
			{
				const decode = b => b ? new TextDecoder ().decode (b) : null;
				const config = decode (await installer.read ('config.txt'));
				const cmdline = decode (await installer.read ('cmdline.txt'));
				writeForm (parseSettings (config, cmdline, readForm ()));
				cardHost = parseSettings (null, cmdline, DEFAULTS).host;
				$('settings-source').textContent = 'Read from the card.';
			}
		}
		else
		{
			add ('SD card', facts.join (' · '));
			status ('card-status', 'The card has no FAT file system, which the Zero needs to start from it. '
				+ 'Formatting erases everything on it.', 'warn');
			$('format').hidden = false;
		}
	}
	updateButtons ();
}

// pico-gpu on the card: its build (version, time, configuration), or how much
// of it is there
function describeInstalled ()
{
	const have = FIRMWARE.filter (n => card.files.has (n));
	if (have.length < FIRMWARE.length)
	{
		return have.length ? `partly on the card (${have.join (', ')})` : 'not on the card';
	}
	const b = card.build;
	if (!b)
	{
		return 'on the card (an older build that doesn\'t say which)';
	}
	const built = b.built.replace ('T', ' ').replace (/Z$/, ' UTC');
	const config = b.config.split (',').map (s => s.replace (/_/g, ' ')).join (', ');
	return `${b.version}, built ${built} (${config})`;
}

async function format ()
{
	if (!confirm (`Erase everything on the ${mb (card.sizeMB)} card in the Zero and make it one FAT32 partition?`))
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
		const files = settingsOnly ? new Map () : await loadFirmware ();
		const all = [...files.entries (),
			     ['config.txt', new TextEncoder ().encode (makeConfig (s))],
			     ['cmdline.txt', new TextEncoder ().encode (makeCmdline (s))]];
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

		status ('action-status', 'Written. Restarting the Zero from the card…');
		const screen = await restartZero (() => installer.reboot ());
		status ('action-status', `Done: pico-gpu started from the card.${screen ? ' ' + screen + '.' : ''}`, 'ok');
		status ('connect-status', 'Connected to pico-gpu.', 'ok');
	}, 'action-status');
	$('progress').hidden = true;
}

// restart the Zero (how: a function) and connect again; returns its
// "Screen: ..." line
async function restartZero (how)
{
	const waiting = waitForSerial (30000);
	await how ();
	const port = await waiting;
	if (!port)
	{
		throw new Error ('The Zero didn\'t come back from its card within 30 s');
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
	await refreshCard (false);			// (its answer comes after the replayed log)
	return seen.pop ();
}

// ---- gears ----------------------------------------------------------------------

let gears = null;			// the running demo's GLStream

async function runGears ()
{
	if (cardHost === 'i2s')
	{
		status ('action-status', 'The card has Host: I2S, so the Zero ignores commands from this page. '
			+ 'Choose Auto or USB above and save the settings first.', 'warn');
		return;
	}
	await run (async () =>
	{
		const connection = installer;
		gears = new GLStream (bytes => connection.writeRaw (bytes));
		connection.rawSink = chunk => gears.push (chunk);
		$('stop-gears').hidden = false;
		status ('action-status', 'Starting…');
		let code;
		try
		{
			code = await runDemo ('gears', gears, line =>
			{
				if (line.trim ())
				{
					log (`(gears) ${line}`);
				}
				const m = line.match (/^gears: (.*fps.*)$/);
				if (m || / running$/.test (line))
				{
					status ('action-status', m ? `Running: ${m[1]}.` : 'Running.', 'ok');
				}
			});
		}
		finally
		{
			connection.rawSink = null;
			gears = null;
			$('stop-gears').hidden = true;
		}

		// the Zero keeps taking commands from the serial port until it restarts
		status ('action-status', code ? 'Gears ended with an error (see the log). Restarting the Zero…'
				       : 'Stopped. Restarting the Zero…', code ? 'bad' : '');
		await restartZero (() => connection.restart ());
		status ('action-status', code ? 'Gears ended with an error (see the log); the Zero restarted.'
				       : 'Stopped; the Zero restarted.', code ? 'bad' : 'ok');
		status ('connect-status', 'Connected to pico-gpu.', 'ok');
	}, 'action-status');
}

// ---- reset ----------------------------------------------------------------------

// restart the Zero from its card (the reboot magic: in any mode, a running
// GL stream too) and connect again
async function reset ()
{
	await run (async () =>
	{
		status ('action-status', 'Restarting the Zero from its card…');
		const connection = installer;
		const screen = await restartZero (() => connection.restart ());
		status ('action-status', `The Zero restarted.${screen ? ' ' + screen + '.' : ''}`, 'ok');
		status ('connect-status', 'Connected to pico-gpu.', 'ok');
	}, 'action-status');
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
	$('save-settings').disabled = busy || !fat || !FIRMWARE.every (n => card.files.has (n));
	$('connect').disabled = busy;
	$('boot').disabled = busy;
	$('format').disabled = busy;
	$('card-retry').disabled = busy;
	$('run-gears').disabled = busy || !installer;
	$('reset').disabled = busy || !installer;

	// the board, its settings and the actions: while a board is connected
	// (kept while an action restarts it)
	if (!busy)
	{
		for (const id of ['card-section', 'settings-section', 'actions-section'])
		{
			$(id).hidden = !(installer && card);
		}
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
	$('format').onclick = format;
	$('card-retry').onclick = () => run (() => refreshCard (true));
	$('install').onclick = () => install (false);
	$('save-settings').onclick = () => install (true);
	$('run-gears').onclick = runGears;
	$('stop-gears').onclick = () => gears?.stop ();
	$('reset').onclick = reset;
	loadManifest ();
	updateButtons ();
	if ('usb' in navigator && 'serial' in navigator)
	{
		noteKnownPort ();
	}
}

// a pico-gpu this page may use already (the user picked it before)
async function noteKnownPort ()
{
	const known = (await navigator.serial.getPorts ()).filter (p => matchesSerial (p));
	if (known.length)
	{
		log ('(page) a pico-gpu the page knows is connected: press Connect');
	}
}

init ();
