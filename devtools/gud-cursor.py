#!/usr/bin/env python3
#
# gud-cursor.py - a mouse pointer on the Zero's USB monitor (GUD) under GNOME
# 50.1, while that GNOME lacks it.
#
# The monitor has no cursor plane (GUD has none), so mutter would have to draw
# the pointer into its frames; mutter 50.1 does that only when no monitor at
# all has a hardware cursor (meta_cursor_renderer_native_update_cursor returns
# !cursor_hw_managed; newer mutter decides it per monitor). A screencast of a
# monitor with the cursor embedded inhibits hardware cursors while the pointer
# is on that monitor (meta-screen-cast-monitor-stream-src.c: inhibit_hw_cursor,
# is_cursor_inhibited = is_cursor_in_stream): then mutter draws it, into the
# monitor's frames too. So this records the pico-gpu monitor (mutter's
# org.gnome.Mutter.ScreenCast, cursor-mode 1: embedded) and consumes the
# stream into nothing (GStreamer); the other monitors keep their hardware
# cursors. GNOME shows its screen-sharing indicator meanwhile.
#
#   devtools/gud-cursor.py          until Ctrl-C: records whenever the monitor is
#                                   there and the screen is unlocked
#
import signal, subprocess, sys
import gi
gi.require_version ('Gio', '2.0')
gi.require_version ('GLibUnix', '2.0')
from gi.repository import Gio, GLib, GLibUnix

MUTTER = 'org.gnome.Mutter.ScreenCast'
PRODUCT = 'pico-gpu'			# the monitor's EDID product name (gpu/display/gud_display)

bus = Gio.bus_get_sync (Gio.BusType.SESSION)
loop = GLib.MainLoop ()
consumer = None

def call (name, path, interface, method, args, reply):
	return bus.call_sync (name, path, interface, method, args,
			      GLib.VariantType (reply) if reply else None,
			      Gio.DBusCallFlags.NONE, -1, None).unpack ()

def find_connector ():
	state = call ('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
		      'org.gnome.Mutter.DisplayConfig', 'GetCurrentState', None,
		      None)
	for (connector, vendor, product, serial), modes, props in state[1]:
		if product == PRODUCT:
			return connector
	return None

def stop (*_):
	end_recording ()
	loop.quit ()

recording = None				# (session path, subscription ids)

def end_recording ():
	global consumer, recording
	if consumer and consumer.poll () is None:
		consumer.terminate ()
	consumer = None
	if recording:
		for sid in recording[1]:
			bus.signal_unsubscribe (sid)
		try:
			call (MUTTER, recording[0], MUTTER + '.Session', 'Stop', None, None)
		except GLib.Error:
			pass			# (closed already)
	recording = None

def start_recording (connector):
	global recording
	session, = call (MUTTER, '/org/gnome/Mutter/ScreenCast', MUTTER, 'CreateSession',
			 GLib.Variant ('(a{sv})', ({},)), '(o)')
	stream, = call (MUTTER, session, MUTTER + '.Session', 'RecordMonitor',
			GLib.Variant ('(sa{sv})', (connector, {'cursor-mode': GLib.Variant ('u', 1)})), '(o)')

	def on_stream_added (connection, sender, path, interface, name, params):
		global consumer
		node, = params.unpack ()
		print ('gud-cursor: %s recorded (PipeWire node %u): the pointer shows there' % (connector, node),
		       flush=True)
		consumer = subprocess.Popen (['gst-launch-1.0', '-q', 'pipewiresrc', 'path=%u' % node,
					      'do-timestamp=true', '!', 'fakesink', 'sync=false'])

	def on_closed (*_):
		print ('gud-cursor: the recording ended (screen locked, or the monitor went)', flush=True)
		end_recording ()

	recording = (session, [
		bus.signal_subscribe (MUTTER, MUTTER + '.Stream', 'PipeWireStreamAdded', stream, None,
				      Gio.DBusSignalFlags.NONE, on_stream_added),
		bus.signal_subscribe (MUTTER, MUTTER + '.Session', 'Closed', session, None,
				      Gio.DBusSignalFlags.NONE, on_closed)])
	call (MUTTER, session, MUTTER + '.Session', 'Start', None, '()')

# every 2 s: a recording while the screen is unlocked and the monitor is there
# (mutter refuses screencasts while the screen is locked)
def check ():
	if recording is None:
		connector = find_connector ()
		if connector:
			try:
				start_recording (connector)
			except GLib.Error as e:
				end_recording ()
				if 'inhibited' not in e.message:
					print ('gud-cursor: %s' % e.message, flush=True)
	return True

def main ():
	print ('gud-cursor: waiting for the %s monitor and an unlocked screen' % PRODUCT, flush=True)
	check ()
	GLib.timeout_add_seconds (2, check)
	for number in (signal.SIGINT, signal.SIGTERM):
		GLibUnix.signal_add (GLib.PRIORITY_DEFAULT, number, stop)
	loop.run ()

main ()
