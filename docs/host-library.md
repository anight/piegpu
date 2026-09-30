# The host library (`libpgpu`)

How the host side works: the library every host board shares, the GL ES API
on top of the protocol ([protocol.md](protocol.md)), and how it maps to the
wire. *Informative*: the protocol is the contract; this is one host's way of
using it.

The host library (`libpgpu/`, board independent) has two layers:

- **`pgpu`** (`libpgpu/pgpu.{h,c}`): one C function per command. It
  batches packets, splits large uploads, turns client-side arrays into
  `PROGRAM_DRAW_INLINE`, and parses replies into queues (`ERROR` replies
  separately, `pgpu_poll_error`), including the reply stream of an I2S link
  (`pgpu_rx_parse`, [protocol](protocol.md) §9.1). The words go through a transport
  (`libpgpu/pgpu_link.h`, one per link in `transports/`): on the Pico the I2S
  link (`transports/pico-i2s`: DMA over PIO, READY before each batch, replies
  sampled into a DMA ring and parsed in a 1 ms timer); on a PC the RPi's USB
  (`transports/pc-usb`, [pgl on a PC](#pgl-on-a-pc)). The builds per host board are in `hosts/`.
- **`pgl`** (`libpgpu/gles/pgl.{h,c}`): the **OpenGL ES 2.0 API**, with
  the **GL ES 1.1 fixed-function calls** for program 0. It keeps the GL state
  (for `glGet*`, `glIsEnabled`, object names) and encodes it into commands.
  `gltest.c` (self test 8) exercises it using only `gl*` calls, on the Pico and
  on a PC.
- **Video** ([protocol](protocol.md) §7.12): `pglVideoTexture (texture, stream, width, height,
  coded_width, coded_height, avcc, avcc_bytes)` makes a GL texture name a
  video texture (`VIDEO_OPEN`; pgl then knows it as a complete RGBA texture,
  linear, clamped); `pglVideoResize (texture, stream, width, height)` changes
  its size, the stream going on (`RESIZE`). `pgpu_video_room` says how big a sample may be now (from
  the last `VIDEO_STATUS`, which the library keeps per stream like `DISPLAY`,
  and what it has sent since); `pgpu_video_control` and
  `pgpu_video_get_status` the rest.
- **Audio** ([protocol](protocol.md) §7.13): `pgpu_audio_open (video_stream,
  asc, asc_bytes)` opens the audio stream (stream `PGPU_AUDIO_STREAM`, 3) for
  AAC with its AudioSpecificConfig and makes it the clock of that video
  stream; its samples, room and status go through the video calls with
  stream 3 (`pgpu_video_sample_read`, `pgpu_video_room`,
  `pgpu_video_get_status`); `pgpu_audio_volume (percent)` sets the volume.
- **The data path from a file:** a file is read through a callback
  (`pgpu_read_t`: bytes at an offset), so it needn't be in memory: on an SD
  card the filesystem reads its sectors as needed.
  - `libpgpu/pgpu_mp4.{h,c}` reads an MP4's H.264 track through it (or,
    `pgpu_mp4_open_audio`, its AAC track: the `mp4a` entry's `esds`, also
    inside QuickTime's `wave` box, gives the AudioSpecificConfig, its rate
    and channels; each track takes a `pgpu_mp4_t` of its own): the boxes
    by their headers (the `moov` may follow the media data), the sample
    tables through 256-byte windows (about 1.5 KB of state however long the
    file; tables read in order, so each window once): each sample's offset,
    size, times (the edit list applied) and keyframe flag, and the avcC.
    Checked against ffprobe on five files (moov at the end and first, an
    interleaved AAC track, no B-frames, 2880 samples): every offset, size and
    keyframe the same, times within 1 µs, read in 512-byte sectors (8 … 26
    sectors to open a file).
  - `pgpu_video_sample_read` sends a sample in `VIDEO_DATA` packets, reading
    each packet's data through the callback straight into the packet (the
    samples go as they are: format AVCC). So the only copy on the host is the
    filesystem's, from its sector buffer (or the card's DMA) into the
    packet; no buffer for a sample. A read that fails leaves the sample
    unfinished; the RPi drops it at the next sample's first chunk.
    The reads are arranged for a filesystem's DMA: a sample's first packet
    carries the bytes up to the file's next 512-byte sector boundary, the
    others whole sectors, and idle words before a packet put its data on a
    64-byte boundary (`PGPU_READ_ALIGN`: the ESP32-P4's cache line; the
    staging buffers are aligned so too). So the filesystem reads whole sectors
    by DMA straight into the packet. Unaligned, ESP-IDF's SD driver allocated
    a DMA buffer the size of the read, read into it and copied it over, on
    every read.
  - `demos/video.c` plays the file `PGPU_VIDEO_PATH` if the host has a
    filesystem (POSIX `open`/`lseek`/`read`), else an MP4 linked into the
    host's image (memory as the file, `pgpu_mp4_open_memory`). When the screen
    changes (panel ↔ HDMI) it resizes the texture; the video keeps its place.
    If the file has an AAC track, its samples go to the audio stream the same
    way, in time order with the video's, and the video follows the sound's
    clock. Both tracks loop with the same period, the longer track's (the Big
    Buck Bunny trailer: video 32.48 s, audio 32.98 s), so they stay together
    round after round.
    `hosts/pc/videoplay` plays a file on the PC, through a reader that does as
    an SD filesystem does (whole sectors, a one-sector cache).
  - **The ESP32-P4's microSD card** (`hosts/esp32p4/main/sdcard.c`, the video
    app): SDMMC slot 0, 4 bits at 40 MHz (CLK GPIO43, CMD 44, D0–D3 39–42);
    the card's supply is on-chip LDO channel 4 through a P-MOSFET that GPIO45
    switches on (low). FAT32 at `/sdcard` (ESP-IDF 5.5's FatFs has exFAT
    off), the file `/sdcard/video1.mp4` (CMake `PGPU_VIDEO_FILE`). Measured
    on a 64 GB card: POSIX `read` into the packet 12.8 MB/s (16.4 MB/s into
    DMA-capable memory); unbuffered stdio (`fread` with no buffer) managed
    84 KB/s, buffered 2.1 MB/s. With stdio a 1920×800 24 fps film (1.9 GB,
    102 minutes) played slowly with the P4's CPU at 100%; with POSIX reads
    it plays at 24 fps. FatFs' fast seek (`CONFIG_FATFS_USE_FASTSEEK`) is on
    for the seeks between the sample tables and the samples; alone it didn't
    help stdio.
  - **The card sometimes stalls:** stretches of 5–10 s in which every read
    takes about 25 ms, whatever its size (2–15 KB), dropping frames. Seen with
    the unaligned reads (7 in 29 minutes) and with the aligned ones (one in
    the first 17 minutes, at 1020 s into that film, file offsets 327–330 MB).
    Not understood yet.
  - **The link is the limit on the P4:** its chip (revision v1.0) runs I2S
    from the APLL, at most 125 MHz, divided by 2 to MCLK and by 2 again to
    BCLK: 31.25 MHz, 3.9 MB/s (25 MHz before). `linktest` (random buffer
    uploads, and random textures uploaded and read back, compared word by
    word): 25 MHz 3.1 MB/s down, 2.0 MB/s up; 31.25 MHz 3.9 MB/s down, 2.37
    MB/s up, and in 10 minutes 87.4M words there and back with 0 wrong, 0
    CRC errors either way. That film's peaks send about 2 MB/s of video
    (2132 KB in one second), and GL frames wait behind it: its busiest
    stretch (100–126 s) went 22–43 fps with 60 frames dropped at 25 MHz,
    30–60 fps with 35 dropped at 31.25 MHz. The video frames no GL frame
    showed count as dropped.

## How pgl maps GL to the wire

| GL | Wire |
|---|---|
| `glGen*`, `glBind*`, `glDelete*` | GL names map to RPi ids: buffers 1–250, textures 1–110 (texture object 0 of each target: 111, 112), framebuffers 1–16. Colour renderbuffers are RPi textures 121–128; framebuffers without a colour attachment get a scratch colour texture (113–120). Programs get RPi ids 1–64 from a pool. |
| `glBufferData`, `glBufferSubData` | `BUFFER_CREATE`, `BUFFER_DATA`. Buffers up to 16 KB and all index buffers are also kept on the Pico (on a PC all buffers), for draws that combine an index buffer with client-side vertex arrays. |
| `glTexImage2D`, `glTexSubImage2D`, `glCompressedTexImage2D` (ETC1), `glCopyTex[Sub]Image2D`, `glGenerateMipmap`, `glTexParameter*` | `TEXTURE_CREATE` when level 0's size or format changes, then `TEXTURE_DATA` (repacked for `GL_UNPACK_ALIGNMENT`; `glTexSubImage2D` data of another type than the texture's is converted on the Pico), `COPY_TEX_IMAGE`, `GENERATE_MIPMAP`, `TEXTURE_PARAMS`. A texture pgl knows to be incomplete (a level of size 0, a mipmap level that doesn't fit level 0) is bound as none. The GL ES 1.1 `GL_GENERATE_MIPMAP` parameter is supported. |
| `glFramebufferTexture2D`, `glFramebufferRenderbuffer`, `glBindFramebuffer` | `FRAMEBUFFER_CREATE` and `BIND_FRAMEBUFFER`, sent by the next draw, clear or read. A depth or stencil renderbuffer sets the depth and stencil flag and names a shared depth and stencil buffer (the renderbuffer), so framebuffers attaching the same renderbuffer share its contents. |
| `glEnable`, `glDisable` | `ENABLE` / `DISABLE`, sent by the next draw or clear. Depth and stencil tests are off on targets without those buffers, as in GL. |
| `glShaderBinary` + `glLinkProgram`, `glProgramBinaryOES`; on a PC also `glCompileShader` + `glLinkProgram` | `PROGRAM_CREATE` / `PROGRAM_DATA`, then a `PING`. An `ERROR` from checking the blob makes the link fail. Samplers are set to unit 0, as in GL. |
| `glUniform*` | `PROGRAM_UNIFORM`: locations are uniform index << 16 \| array element. Values are converted to the program's types (int32; bool 0 / ~0). Sampler uniforms use `PROGRAM_SAMPLER`. |
| `glVertexAttribPointer`, `glDrawArrays`, `glDrawElements` with a program | Buffer arrays use `ATTRIB_ARRAY`. Client-side arrays and indices use `PROGRAM_DRAW_INLINE`, with the buffer arrays' offsets moved to the first vertex sent; lists are split into several packets; strips, fans and loops too large for a packet go to temporary buffers (RPi ids 253, 254). Draws of more than 65535 vertices are split (lists and strips). |
| GL ES 1.1 arrays and draws | Buffer arrays use `ARRAY`. Client-side arrays (interleaved ones once) and indices are copied into two stream buffers (RPi ids 251 and 252), which the RPi reads when the draw arrives. |
| GL ES 1.1 matrices, lights, material, fog, `glTexEnv`, `glAlphaFunc`, `glShadeModel`, `glColor4f`, `glNormal3f`, `glMultiTexCoord4f` | Matrix stacks kept on the Pico (`LOAD_MATRIX` before a draw that needs a changed matrix); `LIGHT` (position and spot direction transformed by the modelview when set), `MATERIAL`, `LIGHT_MODEL`, `FOG`, `TEX_ENV`, `ALPHA_FUNC`, `SHADE_MODEL`, `COLOR`, `NORMAL`, `TEXCOORD`. |
| `glClear`, `glClearColor`, `glClearDepthf`, `glClearStencil` | `CLEAR` |
| `glReadPixels` (RGBA, UNSIGNED_BYTE) | `READ_PIXELS`, rows at `GL_PACK_ALIGNMENT`. Alpha is 255 on targets without alpha. |
| `glGetError` | Errors found by pgl at once. `ERROR` replies map to GL errors (ENUM → `GL_INVALID_ENUM`, LIMIT → `GL_INVALID_VALUE`, MEMORY → `GL_OUT_OF_MEMORY`, others → `GL_INVALID_OPERATION`). If commands were sent since the last call, `glGetError` first waits for them with a `PING` (about 80 µs over USB, 1–3 ms over I2S), so it reports their errors. `pglGetRPiError` gives the last one's code, opcode and detail; with `PGL_DEBUG` set (PC), pgl prints them. |
| `pglSwapBuffers` | `FRAME_END`. Then, if a `DISPLAY` reply has brought a new screen size: a viewport and scissor box that covered the whole old screen are set to the new one (`VIEWPORT`, `SCISSOR`), as a window system does for a resized window. `pglGetScreenSize` gives the size; `pgpu_get_display` the whole `DISPLAY` reply. |

## Differences from GL ES 2.0 and 1.1

- **No shader compiler on the Pico** (`GL_SHADER_COMPILER` is false), which GL
  ES 2.0 allows when a shader binary format is supported (2.10). Programs are
  compiled by `tools/glslc`; the binary is the `NAME_info` structure of the
  generated header, "an optimized pair of vertex and fragment shaders"
  (2.10.2): load it with `glShaderBinary` (format `PGL_SHADER_BINARY_PGPU`)
  into the vertex and the fragment shader, then `glLinkProgram`; or with
  `glProgramBinaryOES` (`PGL_PROGRAM_BINARY_PGPU`). As the specification
  requires without a compiler, `glShaderSource`, `glCompileShader`,
  `glReleaseShaderCompiler`, `glGetShaderPrecisionFormat`,
  `glGetShaderInfoLog`, `glGetShaderSource` and `glGetShaderiv` for the compile
  status, info log and source lengths report `GL_INVALID_OPERATION`. Attribute
  locations of binaries are those given to glslc, so `glBindAttribLocation` has
  no effect on them. On a PC, pgl compiles ([below](#pgl-on-a-pc)).
- Programs are compiled for triangles, lines and points by default; a program
  compiled with fewer (`glslc -v`) reports `ERROR` 10 for the others.
- Draws of more than 65535 vertices as fans or loops report
  `GL_OUT_OF_MEMORY`.
- **Framebuffers:** colour attachments are level 0 of an RGBA or RGB texture,
  or an RGBA4, RGB5_A1, RGB565, RGB8 or RGBA8 renderbuffer. The RPi keeps
  depth and stencil together: a framebuffer with separate depth and stencil
  renderbuffers shares its depth and stencil with others through the depth one.
- **Textures:** a level of an incomplete texture that doesn't fit level 0
  keeps its old contents on the RPi (GL has none, the texture being
  incomplete); only visible once the texture is complete again.
- **Fixed function:** 4 lights, no spot lights (`GL_SPOT_*` is stored but
  ignored), one texture unit, `MODULATE`, `REPLACE`, `DECAL` and `BLEND`
  environments (no `ADD` or `COMBINE`), no clip planes, no point size. The
  texture coordinate's r and q are ignored.
- **`GL_DITHER`** is stored but has no effect: the V3D's dither moves values
  by up to two steps (0 becomes 2 of 31, measured), where GL requires one of
  the two nearest values (GL ES 2.0 4.1.7). pgl keeps it off (the RPi's
  `DITHER` capability); without it, the V3D truncates, as GL does.
- `GL_POINT_SMOOTH`, `GL_LINE_SMOOTH` and the multisample enables are stored,
  but have no effect (there is no multisample buffer).
- **Mesa's limits** (glslc compiles with Mesa's `vc4`): at most 8 attributes
  including aliased ones (`attribute_location.bind_aliasing.max_cond_*`); the
  V3D loses triangles with vertices far outside the viewport
  (`clipping.triangle_vertex.clip_three.*`). Mesa's `vc4` fails these on a
  Raspberry Pi 3 as well (`src/broadcom/ci/broadcom-rpi3-fails.txt`).

## pgl on a PC

pgl and pgpu also build for Linux (`hosts/pc`) and for a page (`hosts/web`,
WebAssembly): the transport (`transports/pc-usb/pgpu_host.c`) sends the
packets to the RPi over its USB port ([protocol](protocol.md) §13), so
programs on the PC drive the GPU without a microcontroller.

- **The session's end:** `pgpu_stream_end ()` sends `STREAM_END`
  ([protocol](protocol.md) §13): the RPi resets and its serial port carries
  text again. The serial transport sends it at `exit` and, as a ready-made
  packet written by a signal handler, on SIGINT, SIGTERM and SIGHUP; the
  page's Stop sends it too (`web/installer/pgi.js`, `endStream`) and waits
  for the RPi's `#STREAM END`, restarting the RPi only without it.

- **Shader compiler:** on the PC, `GL_SHADER_COMPILER` is true:
  `glCompileShader` runs `glslc --check` (Mesa's info log),
  `glLinkProgram` runs glslc for the pair with the attribute locations bound
  by `glBindAttribLocation` (others get the lowest free ones; aliases and
  matrix attributes as GL) and loads the result as a binary. Results are
  cached (`~/.cache/pgpu-glslc`).
- **Tests:** `gltest_host` (self test 8), `compile_test`, and the Khronos
  conformance tests: `tools/deqp/build-deqp.sh` builds dEQP-GLES2 (VK-GL-CTS)
  with a `pgl` platform (the panel as a 320x240 RGB565 window with 24-bit depth
  and 8-bit stencil); `tools/deqp/run-deqp.py` runs cases in batches (going on
  after a crash), `failures.py` groups the failures, `compare-vc4.py` compares
  with Mesa's `vc4` on a Raspberry Pi 3.
