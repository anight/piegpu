// settings.js - the piegpu settings as the RPi reads them: config.txt (the
// firmware's: memory, clocks, the HDMI mode) and cmdline.txt (the gpu app's
// options, README "Kernel command line"), and back from the files on a card.

export const DEFAULTS = {
	host: 'auto',		// GL commands from: auto (a PC over USB once it streams, else I2S), usb, i2s
	gud: 'on',		// on: a USB monitor for a PC's desktop (GUD); off: none
	output: 'auto',		// auto: HDMI while a monitor is connected, else the panel
	panel: 'auto',		// auto: detect over SDO (MISO); yes; none
	hdmiPixels: 0,		// 0: the monitor's native resolution
	hdmiMode: 'custom',	// firmware | monitor | custom
	cvtWidth: 1024,
	cvtHeight: 600,
	cvtRate: 60,
};

export function makeCmdline (s)
{
	const options = [`host=${s.host}`, `gud=${s.gud}`, `output=${s.output}`, `panel=${s.panel}`];
	if (s.hdmiPixels > 0)
	{
		options.push (`hdmi_pixels=${s.hdmiPixels}`);
	}
	return options.join (' ') + '\n';
}

// the boards piegpu supports for now, and is built for (web/installer/
// make-firmware.sh); more RPi boards are to come
export const BOARDS = {zero: 'Raspberry Pi Zero / Zero W', zero2: 'Raspberry Pi Zero 2 W'};

// board: 'zero' (32 bit, kernel.img), 'zero2' (64 bit, kernel8.img), or 'any'
// (a blank board started over USB: which it is shows only then; the Pi
// firmware's [pi02] filter picks the Zero 2 W's lines)
// the Zero 2 W's V3D at 400 MHz (its default: 300): 13-24% faster rendering;
// dEQP's 2015-case subset as at 300, the SoC under 60 C after 5 minutes of
// the heaviest demo (measured 2026-09-29). The Zero stays at its default.
const V3D_400 = ['# the GPU (V3D) at 400 MHz, not 300: 13-24% faster (tested)', 'v3d_freq=400', 'v3d_freq_min=400'];

export function makeConfig (s, board = 'zero')
{
	const lines = [
		board === 'any' ? '# piegpu on a Raspberry Pi Zero or Zero 2 W (the installer page, starting it over USB)'
				: `# piegpu on a ${BOARDS[board]} (written by the installer page)`,
		...(board === 'zero2' ? ['# 64 bit: the firmware starts kernel8.img', 'arm_64bit=1', ...V3D_400] : ['arm_64bit=0']),
		'initial_turbo=0',
		'# memory for the VideoCore (its video decoder)',
		'gpu_mem=128',
		'# core clock 300 MHz: the panel\'s SPI runs at 75 MHz (300 / 4)',
		'core_freq=300',
		'core_freq_min=300',
		'# HDMI stays on without a monitor at boot, so one plugged in later gets',
		'# a picture (piegpu watches the hot-plug line); no overscan borders',
		'hdmi_force_hotplug=1',
		'disable_overscan=1',
	];
	if (s.hdmiMode === 'monitor')
	{
		lines.push ('# HDMI mode: the monitor\'s preferred computer (DMT) mode',
			    'hdmi_group=2');
	}
	else if (s.hdmiMode === 'custom')
	{
		lines.push (`# HDMI mode: ${s.cvtWidth}x${s.cvtHeight} at ${s.cvtRate} Hz (CVT)`,
			    'hdmi_group=2',
			    'hdmi_mode=87',
			    `hdmi_cvt=${s.cvtWidth} ${s.cvtHeight} ${s.cvtRate} 6 0 0 0`);
	}
	else
	{
		lines.push ('# HDMI mode: the firmware chooses (it prefers TV modes)');
	}
	if (board === 'any')
	{
		lines.push ('[pi02]', '# a Zero 2 W: 64 bit, kernel8.img', 'arm_64bit=1', ...V3D_400, '[all]');
	}
	return lines.join ('\n') + '\n';
}

// the settings in a card's files (either may be null); what's missing stays
// as in base
export function parseSettings (config, cmdline, base = DEFAULTS)
{
	const s = {...base};
	for (const option of (cmdline || '').trim ().split (/\s+/))
	{
		const [key, value] = option.split ('=');
		if (key === 'host' && ['auto', 'usb', 'i2s'].includes (value))
		{
			s.host = value;
		}
		else if (key === 'gud' && ['on', 'off'].includes (value))
		{
			s.gud = value;
		}
		else if (key === 'output' && ['auto', 'panel', 'hdmi'].includes (value))
		{
			s.output = value;
		}
		else if (key === 'panel' && ['auto', 'yes', 'none'].includes (value))
		{
			s.panel = value;
		}
		else if (key === 'hdmi_pixels' && Number (value) > 0)
		{
			s.hdmiPixels = Number (value);
		}
	}
	if (config !== null && config !== undefined)
	{
		const get = key => (config.match (new RegExp (`^\\s*${key}\\s*=\\s*(.+?)\\s*$`, 'm')) || [])[1];
		const cvt = get ('hdmi_cvt');
		if (get ('hdmi_mode') === '87' && cvt)
		{
			const [w, h, r] = cvt.split (/\s+/).map (Number);
			Object.assign (s, {hdmiMode: 'custom', cvtWidth: w, cvtHeight: h, cvtRate: r});
		}
		else
		{
			s.hdmiMode = get ('hdmi_group') === '2' ? 'monitor' : 'firmware';
		}
	}
	return s;
}
