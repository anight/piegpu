// app.js - the installer page: connects to the RPi (Web Serial to a running
// piegpu, or WebUSB to start a blank RPi first: rpiboot.js), shows its
// card, and writes the firmware and the settings onto it (pgi.js).

import {USB_FILTERS, bootStage, waitForBootDevice} from './rpiboot.js';
import {SERIAL_FILTERS, Installer, crc32} from './pgi.js';
import {BOARDS, DEFAULTS, makeCmdline, makeConfig, parseSettings} from './settings.js';
import {GLStream, runDemo} from './gl.js';

const $ = id => document.getElementById (id);
const STORE = 'piegpu-installer-settings';

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
		const boards = Object.keys (manifest.boards || {}).map (b => BOARDS[b]).join (' and ');
		$('version').textContent = `Firmware ${manifest.version} for the ${boards}, built ${manifest.built}.`;
	}
	catch (e)
	{
		$('version').textContent = 'The firmware files are missing (web/installer/make-firmware.sh).';
	}
}

// 'zero' or 'zero2': the board a running piegpu reports (INFO; an older one
// doesn't say: a Zero); null before one has answered. Only these two are
// supported for now (BOARDS): another board counts as a Zero, and the board
// facts say it isn't supported
function currentBoard ()
{
	if (!card)
	{
		return null;
	}
	return /Zero 2/.test (card.board) ? 'zero2' : 'zero';
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
// config.txt's [pi02] lines), plus this page's settings
async function bootFiles ()
{
	const files = new Map ();
	for (const board of Object.keys (manifest?.boards || {zero: null}))
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
		await refreshCard (true);
		status ('connect-status', 'Connected to piegpu (restarted).', 'ok');
	}
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
	};
	$('card-retry').hidden = true;
	$('format').hidden = true;
	add ('Type', card.board ? `${card.board}${card.ramMB ? `, ${card.ramMB} MB` : ''} (revision ${card.revision})`
				  + (/Zero/.test (card.board) ? '' : ': not supported yet (for now: the Zero / Zero W and the Zero 2 W)')
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
			status ('card-status', 'The card has no FAT file system, which the RPi needs to start from it. '
				+ 'Formatting erases everything on it.', 'warn');
			$('format').hidden = false;
		}
	}
	updateButtons ();
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
	const built = b.built.replace ('T', ' ').replace (/Z$/, ' UTC');
	const config = b.config.split (',').map (s => s.replace (/_/g, ' ')).join (', ');
	return `${b.version}, built ${built} (${config})`;
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
		const all = [...files.entries (),
			     ['config.txt', new TextEncoder ().encode (makeConfig (s, board))],
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
	await refreshCard (false);			// (its answer comes after the replayed log)
	return seen.pop ();
}

// ---- demos on the RPi -----------------------------------------------------------

// the video the Test video button plays (web/installer/make-firmware.sh puts
// it here: the Sintel trailer, Blender Foundation, CC BY 3.0)
const TEST_VIDEO = 'videos/sintel_trailer-480p.mp4';

let demo = null;			// the running demo's GLStream

async function testOpenGL ()
{
	await runOnRPi ('gears', 'The OpenGL test', async () => ({}));
}

async function testVideo ()
{
	await runOnRPi ('video', 'The video', async () =>
	{
		status ('action-status', 'Fetching the test video…');
		const response = await fetch (TEST_VIDEO);
		if (!response.ok)
		{
			throw new Error (`No test video on this page (${TEST_VIDEO}: web/installer/make-firmware.sh)`);
		}
		return {'/video.mp4': new Uint8Array (await response.arrayBuffer ())};
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
				const m = line.match (new RegExp (`^${name}: (.*fps.*)$`));
				if (m || / running$/.test (line))
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
		}

		status ('action-status', code ? `${title} ended with an error (see the log). Restarting the RPi…`
				       : 'Stopped. Restarting the RPi…', code ? 'bad' : '');
		await restartRPi (() => connection.restart ());
		status ('action-status', code ? `${title} ended with an error (see the log); the RPi restarted.`
				       : 'Stopped; the RPi restarted.', code ? 'bad' : 'ok');
		status ('connect-status', 'Connected to piegpu.', 'ok');
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
	$('run-gears').onclick = testOpenGL;
	$('test-video').onclick = testVideo;
	$('stop-demo').onclick = () => demo?.stop ();
	$('reset').onclick = reset;
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
