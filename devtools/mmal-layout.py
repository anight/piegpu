#!/usr/bin/env python3
#
# mmal-layout.py - the layout of MMAL's messages to the VideoCore as the 32-bit
# ARM compiler makes it (the VideoCore's side): each struct's size and each
# field's offset and size, from offsetof/sizeof in a probe compiled with the
# gpu app's flags for MMAL (make -n) and read back from the object file.
# gpu/video/userland/interface/mmal/vc/mmal_vc_msgs.h checks its fixed-width
# layout against these numbers, so 64-bit clients send the same bytes.
#
#   devtools/mmal-layout.py
#
import os, struct, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
GPU = os.path.join(ROOT, 'gpu')
fields = [
 ('mmal_worker_msg_header', ['magic','msgid','control_service','u.waiter','status','dummy']),
 ('mmal_worker_version', ['header','flags','major','minor','minimum']),
 ('mmal_worker_component_create', ['header','client_component','name','pid']),
 ('mmal_worker_component_create_reply', ['header','status','component_handle','input_num','output_num','clock_num']),
 ('mmal_worker_component_destroy', ['header','component_handle']),
 ('mmal_worker_component_enable', ['header','component_handle']),
 ('mmal_worker_port_info_get', ['header','component_handle','port_type','index']),
 ('mmal_worker_port_info_set', ['header','component_handle','port_type','index','port','format','es','extradata']),
 ('mmal_worker_port_info', ['header','status','component_handle','port_type','index','found','port_handle','port','format','es','extradata']),
 ('mmal_worker_reply', ['header','status']),
 ('mmal_worker_port_action', ['header','component_handle','port_handle','action','param','param.enable.port','param.connect.component_handle','param.connect.port_handle']),
 ('mmal_worker_port_param_set', ['header','component_handle','port_handle','param','space']),
 ('mmal_worker_port_param_get_reply', ['header','status','param','space']),
 ('struct MMAL_DRIVER_BUFFER_T', ['magic','component_handle','port_handle','client_context']),
 ('mmal_worker_buffer_from_host', ['header','drvbuf','drvbuf_ref','buffer_header','buffer_header_type_specific','is_zero_copy','has_reference','payload_in_message','short_data']),
 ('mmal_worker_event_to_host', ['header','client_component','port_type','port_num','cmd','length','data','delayed_buffer']),
 ('MMAL_PORT_T', ['priv','name','type','index','index_all','is_enabled','format','buffer_num_min','buffer_size_min','buffer_alignment_min','buffer_num_recommended','buffer_size_recommended','buffer_num','buffer_size','component','userdata','capabilities']),
 ('MMAL_ES_FORMAT_T', ['type','encoding','encoding_variant','es','bitrate','flags','extradata_size','extradata']),
 ('MMAL_VIDEO_FORMAT_T', ['width','height','crop','frame_rate','par','color_space']),
 ('MMAL_BUFFER_HEADER_T', ['next','priv','cmd','data','alloc_size','length','offset','flags','pts','dts','type','user_data']),
 ('MMAL_BUFFER_HEADER_TYPE_SPECIFIC_T', []),
 ('MMAL_ES_SPECIFIC_FORMAT_T', []),
 ('MMAL_PARAMETER_HEADER_T', ['id','size']),
]
names, lines = [], []
for t, fs in fields:
    names.append((t, 'sizeof')); lines.append(f'sizeof ({t}), 0')
    for f in fs:
        names.append((t, f)); lines.append(f'offsetof ({t}, {f}), sizeof ((({t} *) 0)->{f})')
src = '#include <stddef.h>\n#include "interface/mmal/vc/mmal_vc_msgs.h"\nconst unsigned probe[] = {\n' + ',\n'.join(lines) + '\n};\n'
S = tempfile.mkdtemp()
open(f'{S}/probe.c', 'w').write(src)
target = 'video/userland/interface/mmal/vc/mmal_vc_api'
cmd = subprocess.run(['make', '-C', GPU, '-n', '-W', target + '.c', target + '.o'], capture_output=True, text=True).stdout
line = next(l for l in cmd.splitlines() if 'gcc' in l and target in l and not l.startswith('echo'))
flags = line.split(' -c -o ')[0].split()
r = subprocess.run(flags + ['-c', '-o', f'{S}/probe.o', f'{S}/probe.c'], capture_output=True, text=True, cwd=GPU)
if r.returncode: print(r.stderr[:3000]); sys.exit(1)
objcopy = flags[0].replace('gcc', 'objcopy')
subprocess.run([objcopy, '-O', 'binary', '-j', '.rodata', f'{S}/probe.o', f'{S}/probe.bin'], check=True)
d = open(f'{S}/probe.bin', 'rb').read()
vals = struct.unpack('<%dI' % (len(d) // 4), d)
for i, (t, f) in enumerate(names):
    o, s = vals[2*i], vals[2*i+1]
    print(f'{t:40} {f:34} ' + (f'size {o}' if f == 'sizeof' else f'@{o:4} +{s}'))
