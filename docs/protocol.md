# Pico → Zero GPU command protocol, version 1

This document specifies the link between the **Pico 2 W** (the host, which runs
the application) and the **Raspberry Pi Zero** (the GPU, which renders with the
VideoCore IV V3D and drives the ST7789 panel).

Version 1 implements a **fixed-function pipeline in the style of OpenGL ES 1.1**
(option A). No shader compiler is involved: the Zero uses a small set of built-in
QPU programs. The opcode space is laid out so that programmable shaders
(option B: precompiled program blobs and uniforms) can be added later without
changing the framing.

Status: **draft**. Everything is normative unless marked *informative*.

---

## 1. Layers

| Layer | What it defines |
|---|---|
| Physical | pins, I2S timing, the READY and FRAME signals (§2, §3) |
| Packet | framing of 32-bit words: header, payload, CRC32 (§4) |
| Command | execution model, opcodes, payloads, replies and enums (§6–§10) |
| API (Pico library) | a GL ES 1.1-like C API that encodes commands (§13, informative) |

---

## 2. Physical layer

### 2.1 Pins

| Signal | Direction | Pico 2 W pin (GPIO) | Zero pin (GPIO, function) |
|---|---|---|---|
| DATA | Pico → Zero | 21 (GP16) | 38 (GPIO20, PCM_DIN) |
| BCLK | Pico → Zero | 22 (GP17) | 12 (GPIO18, PCM_CLK) |
| FS | Pico → Zero | 24 (GP18) | 35 (GPIO19, PCM_FS) |
| REPLY | Zero → Pico | 25 (GP19) | 40 (GPIO21, PCM_DOUT) |
| READY | Zero → Pico | 26 (GP20) | 36 (GPIO16) |
| FRAME | Zero → Pico | 27 (GP21) | 37 (GPIO26) |
| GND | — | 23, 28 | 39, 34 |

READY has an external 10 kΩ pull-down to GND on the Pico side (see §3.1).
All signals are 3.3 V push-pull.

### 2.2 I2S format

The Pico is the I2S **master**: it drives BCLK and FS. The Zero's PCM block is
clock and frame-sync **slave** for both directions.

- **Frame:** 64 BCLK periods, two 32-bit channels, standard I2S. FS is low for
  channel 1 and high for channel 2, and changes one BCLK before each channel's MSB.
- **Bit order:** MSB first.
- **Timing:** data and FS change while BCLK is low; the receiver samples on the
  rising edge of BCLK.
- **Payload mapping:** each channel slot carries one **stream word**. Channel 1,
  then channel 2, gives two consecutive words, so the stream is a plain sequence
  of 32-bit words. Stereo has no meaning here.
- **Bit clock:** up to **75 MHz** (the Pico 2 W at 150 MHz system clock, PIO at
  2 cycles per bit). That's 75 Mbit/s = **9.375 MB/s** of payload in each
  direction. Measured on the hardware: zero bit errors at 1–75 MHz, and the
  stream is word-aligned.
- **The clock runs continuously.** The Pico keeps BCLK and FS running and sends
  idle words (§4.4) when it has nothing to send. This is required: the Zero can
  only reply (REPLY, §9) while the Pico clocks, and a steady clock keeps the
  Zero's receiver in frame lock.

### 2.3 Direction of the data lines

- DATA (GP16 → PCM_DIN) carries the **command stream**.
- REPLY (PCM_DOUT → GP19) carries the **reply stream**, clocked by the same
  BCLK/FS. The Zero changes REPLY on the falling edge of BCLK.
- *Informative (v1 Pico library):* a second PIO state machine samples GP19 once per
  bit, in lockstep with the one that drives BCLK (same clock divider, started in
  the same cycle, input synchroniser bypassed). It doesn't track FS, so the reply
  words arrive with an unknown bit offset. The parser finds each packet's
  alignment from its header (§9.1). Both sampling phases (at the rising and at the
  falling BCLK edge) were measured error-free at 75 MHz.

---

## 3. Side-band signals

### 3.1 READY (Zero → Pico): flow control

- READY **high** means the Zero's command ring buffer has room for at least
  **one maximum-size packet** (§4.3) plus margin. The Pico may start sending a packet.
- READY **low** means the Pico must not **start** a new packet. A packet already
  started is always completed; the READY rule guarantees there's room for it.
- The Pico samples READY **before sending each packet header**.
- Hysteresis (*informative*, v1 implementation): the Zero drops READY when free
  space falls below 128 KB and raises it again at 256 KB free, out of a 1 MB ring.
- **Reset safety:** while the Zero boots or reboots, GPIO16 is an input with a
  pull-down. READY then reads **low**, so the Pico never streams into a Zero that
  isn't running. The external 10 kΩ pull-down covers an RP2350 GPIO pull-down
  erratum (E9), where an undriven pin can float high with only the internal pull-down.
- After the Zero comes up, the Pico must send `RESET` (§7.1) before anything else.

### 3.2 FRAME (Zero → Pico): frame pacing

The Zero drives FRAME high for at least 10 µs when a frame starts its transfer to
the panel (the moment `FRAME_END`'s image is handed to the panel DMA). The Pico
can use a GPIO edge interrupt to pace its main loop without decoding REPLY.
FRAME is optional for the Pico to use; the Zero always drives it.

---

## 4. Packet layer

### 4.1 Packet format

A packet is a sequence of 32-bit words:

```
word 0         header
word 1 .. N    payload (N = LENGTH words, 0 <= N <= 16384)
word N+1       CRC32
```

Header:

| Bits | Field | Meaning |
|---|---|---|
| 31–24 | SYNC | `0xA5` for commands (Pico → Zero), `0x5A` for replies (Zero → Pico) |
| 23–16 | OPCODE | command or reply code (§6, §9) |
| 15–0 | LENGTH | payload length in 32-bit words |

### 4.2 CRC32

- The algorithm is CRC-32 as used by zlib and Ethernet: reflected polynomial
  `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`.
- It covers the header and payload words, each taken as **4 little-endian bytes**.
  In C, that's the words exactly as they lie in memory on either processor.
- *Informative:* the RP2350's DMA sniffer computes this CRC in hardware while the
  DMA feeds the PIO, so it costs the Pico nothing. On the Zero, a table-driven
  CRC costs roughly 10% of the CPU at the full 9.4 MB/s.

### 4.3 Sizes

- Maximum payload: **16384 words (64 KB)**. Bulk data (buffer or texture uploads)
  larger than that is split into several packets using the offset fields of the
  data commands.
- Maximum packet: 16386 words (header + payload + CRC).

### 4.4 Idle words and resynchronisation

- Between packets, the sender transmits **idle words `0x00000000`**. They're
  never part of a packet and are skipped.
- A receiver looks for a header whose SYNC byte matches its direction, with a
  known opcode and a LENGTH within limits. It then takes LENGTH + 1 more words
  and checks the CRC.
- On a CRC mismatch, the receiver discards the header word only, and resumes the
  search at the next word. This lets it resynchronise after a glitch or a reset
  in the middle of a packet. The Zero also reports the error (`ERROR`, §9).
- Payload words may legitimately look like headers. They're never mistaken for
  one, because a packet is only accepted with a valid CRC.

---

## 5. Data conventions

| Type | Encoding |
|---|---|
| `u32`, `s32` | one word |
| `u16` pairs | packed in one word: first value in bits 15–0, second in bits 31–16 |
| `f32` | IEEE 754 single precision, one word |
| `color` | RGBA8888 in one word: **R in bits 7–0**, G 15–8, B 23–16, A 31–24 (`0xAABBGGRR`) |
| `bytes` | byte data packed little-endian into words, padded with zeros to a whole word |
| `id` | object name chosen by the Pico, `1 .. limit`; 0 means "none" (limits in §11) |

Coordinates follow OpenGL:
- right-handed eye space;
- clip space → NDC in −1 … 1;
- window depth 0 … 1;
- window origin at the **bottom-left** of the viewport;
- matrices column-major, 16 floats.

---

## 6. Execution model

### 6.1 Frames

- Commands between two `FRAME_END` packets form a **frame**. The Zero executes
  commands in stream order. It collects the frame's draws, bins and renders them
  when `FRAME_END` arrives, and hands the image to the panel.
- **State persists across frames**: matrices, enables, bound textures, arrays and
  objects stay as set until changed.
- The Pico may start sending the next frame immediately; the ring buffer absorbs
  it. Back-pressure comes only from READY.

### 6.2 Clearing

The V3D renders in 64×64 tiles, so a clear is part of starting a frame, not a
separate drawing operation.
- `CLEAR` must come **before the first draw of a frame**. A `CLEAR` after a draw
  in the same frame is an error: it's ignored and reported with `ERROR`.
- If a frame has no colour `CLEAR`, the previous frame's colour contents are
  kept. This is slower: the tiles are loaded from memory first.
- Depth is **not** kept between frames in v1: every frame starts with depth
  cleared to the most recent `CLEAR` depth value (1.0 by default).

### 6.3 Object updates within a frame

- `BUFFER_DATA` and `TEXTURE_DATA` take effect for **draws issued after them**,
  with GL semantics. If the object was already used by an earlier draw in the
  same frame, the Zero keeps the old contents for those draws (copy-on-write).
- Deleting an object used by the current frame is allowed. The Zero frees it
  once the frame has rendered.
- *Informative:* frequently changing geometry is best sent with `DRAW_INLINE`.

### 6.4 Errors

- An invalid command is ignored as a whole and reported with an `ERROR` reply.
  Invalid means an unknown opcode, a wrong LENGTH, an out-of-range id or enum, or
  a missing object.
- Errors never stop the stream.

---

## 7. Commands (Pico → Zero, SYNC `0xA5`)

Payload fields are listed word by word. `[n]` means n words.

### 7.1 System

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x01` | RESET | — | Delete all objects and reset all state to the defaults (§7.9). Discard the current frame. Replies `INFO`. |
| `0x02` | GET_INFO | — | Replies `INFO` (§9). |
| `0x03` | PING | `u32 cookie` | Replies `PONG` with the same cookie, once all earlier commands have been **parsed**. |
| `0x04` | GET_STATUS | — | Replies `STATUS` (§9). |

### 7.2 Frame

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x10` | CLEAR | `u32 mask`, `color color`, `f32 depth` | Clear the frame at its start (§6.2). mask bit 0 = colour, bit 1 = depth. |
| `0x11` | FRAME_END | `u32 flags` | End the frame: render and present it. flags: bit 0 = request a `FRAME_DONE` reply. |
| `0x12` | VIEWPORT | `s32 x`, `s32 y`, `u32 width`, `u32 height`, `f32 near`, `f32 far` | Like `glViewport` + `glDepthRangef`. |

### 7.3 Buffers (vertex and index data)

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x20` | BUFFER_CREATE | `id buffer`, `u32 size_bytes` | Create a buffer (contents undefined). Recreating an existing id replaces it. |
| `0x21` | BUFFER_DATA | `id buffer`, `u32 offset_bytes`, `u32 length_bytes`, `bytes data[…]` | Write data. offset and length are multiples of 4. |
| `0x22` | BUFFER_DELETE | `id buffer` | Delete it. |

### 7.4 Textures (texture unit 0 only in v1)

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x28` | TEXTURE_CREATE | `id texture`, `u16 width, u16 height`, `u32 format` | Create a texture. Width and height are powers of two, 1 … 1024. |
| `0x29` | TEXTURE_DATA | `id texture`, `u16 x, u16 y`, `u16 width, u16 height`, `bytes pixels[…]` | Upload a rectangle, rows bottom-up (GL order), tightly packed, each row padded to 4 bytes. ETC1: x, y, width, height are multiples of 4 and the data is ETC1 blocks. |
| `0x2A` | TEXTURE_PARAMS | `id texture`, `u32 min_filter`, `u32 mag_filter`, `u32 wrap_s`, `u32 wrap_t` | Filtering and wrapping (§10.6). |
| `0x2B` | TEXTURE_DELETE | `id texture` | Delete it. |
| `0x2C` | TEXTURE_BIND | `id texture` | Bind to unit 0 (0 = none). |
| `0x2D` | TEX_ENV | `u32 mode`, `color env_color` | Texture environment: MODULATE, REPLACE, DECAL or BLEND (§10.7). |

### 7.5 Fragment state

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x30` | ENABLE | `u32 caps` | Enable the capabilities in the mask (§10.1). |
| `0x31` | DISABLE | `u32 caps` | Disable the capabilities in the mask. |
| `0x32` | DEPTH_FUNC | `u32 func` | §10.2 |
| `0x33` | DEPTH_MASK | `u32 write` | 0 = don't write depth |
| `0x34` | BLEND_FUNC | `u32 src`, `u32 dst` | §10.3 |
| `0x35` | CULL_FACE | `u32 face` | 0 = FRONT, 1 = BACK, 2 = FRONT_AND_BACK |
| `0x36` | FRONT_FACE | `u32 winding` | 0 = CCW, 1 = CW |
| `0x37` | ALPHA_FUNC | `u32 func`, `f32 ref` | alpha test; func as in §10.2 |
| `0x38` | COLOR_MASK | `u32 mask` | bits 0–3 = R, G, B, A writes enabled |

### 7.6 Transform, lighting, fog

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x40` | LOAD_MATRIX | `u32 which`, `f32 m[16]` | Load MODELVIEW (0), PROJECTION (1) or TEXTURE (2), column-major. The matrix **stack** is kept by the Pico library (§13); the wire carries only the resulting matrices. |
| `0x41` | LIGHT | `u32 light`, `f32 position[4]`, `color ambient`, `color diffuse`, `color specular`, `f32 attenuation[3]` | Light 0–3. position w = 0 means directional. attenuation = constant, linear, quadratic. The position is taken as given, **in eye space** (the library transforms it by the current modelview, as GL does). |
| `0x42` | MATERIAL | `color ambient`, `color diffuse`, `color specular`, `color emission`, `f32 shininess` | Front and back material. |
| `0x43` | LIGHT_MODEL | `color ambient`, `u32 two_side` | Global ambient light; two-sided lighting. |
| `0x44` | FOG | `u32 mode`, `color color`, `f32 start`, `f32 end`, `f32 density` | mode: 0 = LINEAR, 1 = EXP, 2 = EXP2 |
| `0x45` | SHADE_MODEL | `u32 mode` | 0 = SMOOTH, 1 = FLAT |
| `0x46` | COLOR | `color color` | Current colour, used when the colour array is disabled. |
| `0x47` | NORMAL | `f32 n[3]` | Current normal, used when the normal array is disabled. |
| `0x48` | TEXCOORD | `f32 st[2]` | Current texture coordinate. |

### 7.7 Vertex arrays and drawing

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x50` | ARRAY | `u32 attribute`, `id buffer`, `u32 offset_bytes`, `u32 stride_bytes`, `u32 size`, `u32 type` | Point an attribute (§10.4) at a buffer. size = components (1–4); type as in §10.5. stride 0 = tightly packed. |
| `0x51` | ARRAYS_ENABLE | `u32 mask` | Set exactly which attributes come from arrays: bit n = attribute n. The others use the current values (§7.6). |
| `0x52` | DRAW_ARRAYS | `u32 mode`, `u32 first`, `u32 count` | Like `glDrawArrays`. |
| `0x53` | DRAW_ELEMENTS | `u32 mode`, `u32 count`, `u32 index_type`, `id buffer`, `u32 offset_bytes` | Like `glDrawElements`. index_type: 0 = u8, 1 = u16. |
| `0x54` | DRAW_INLINE | `u32 mode`, `u32 count`, `u32 attribute_mask`, then `count` vertices | Immediate-mode geometry in the packet itself (§7.8). |

### 7.8 DRAW_INLINE vertex layout

Each vertex holds the attributes selected by `attribute_mask`, in this fixed
order and format:

| Bit | Attribute | Words | Format |
|---|---|---|---|
| 0 | POSITION | 3 | `f32 x, y, z` (w = 1) |
| 1 | COLOR | 1 | `color` |
| 2 | NORMAL | 3 | `f32 x, y, z` |
| 3 | TEXCOORD | 2 | `f32 s, t` |

POSITION is required. Attributes not in the mask use the current values. The
count is limited by the packet size: for example 16384 / 4 = 4096 vertices with
position + colour.

### 7.9 Default state (after power-up and `RESET`)

| State | Default |
|---|---|
| matrices | identity |
| viewport | full panel (0, 0, width, height), depth range 0 … 1 |
| enables | all off |
| depth | func LESS, depth writes on |
| blend | ONE, ZERO |
| cull face / front face | BACK / CCW |
| alpha func | ALWAYS, 0 |
| colour mask | all on |
| current colour / normal / texcoord | (1, 1, 1, 1) / (0, 0, 1) / (0, 0) |
| lights | light 0: diffuse and specular (1, 1, 1, 1); lights 1–3: black; all positions (0, 0, 1, 0); ambient (0, 0, 0, 1); attenuation (1, 0, 0) |
| material | ambient (0.2, 0.2, 0.2, 1), diffuse (0.8, 0.8, 0.8, 1), specular and emission (0, 0, 0, 1), shininess 0 |
| light model | ambient (0.2, 0.2, 0.2, 1), one-sided |
| fog | EXP, colour (0, 0, 0, 0), start 0, end 1, density 1 |
| shade model | SMOOTH |
| texture env | MODULATE, env colour (0, 0, 0, 0) |
| arrays | none enabled, none bound |
| bound texture | none |

---

## 8. Opcode map

| Range | Group |
|---|---|
| `0x00` | never a valid opcode (idle words) |
| `0x01`–`0x0F` | system |
| `0x10`–`0x1F` | frame |
| `0x20`–`0x27` | buffers |
| `0x28`–`0x2F` | textures |
| `0x30`–`0x3F` | fragment state |
| `0x40`–`0x4F` | transform, lighting, fog, current values |
| `0x50`–`0x5F` | arrays and drawing |
| `0x60`–`0x7F` | reserved for fixed-function additions |
| `0x80`–`0xBF` | **reserved for programmable shaders (option B)**: program upload, uniforms, generic attributes |
| `0xC0`–`0xEF` | reserved |
| `0xF0`–`0xFF` | debug and vendor |

Debug commands (v1 Zero implementation, not needed by applications):

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0xF0` | DEBUG_SCREENSHOT | — | The Zero writes the last presented frame to its USB development log (base64 RGB565 between `#SCREENSHOT` and `#END` lines; `devtools/screenshot.py` makes a PNG). |

---

## 9. Replies (Zero → Pico, SYNC `0x5A`)

Replies use the same packet format, sent on REPLY. Idle words are `0x00000000`.
A reply to a request uses the request's opcode (`GET_INFO` → `INFO`, `PING` → `PONG`, …).

| Op | Name | Payload | Sent when |
|---|---|---|---|
| `0x02` | INFO | `u32 version`, `u16 width, u16 height`, `u32 max_texture_size`, `u32 max_buffers`, `u32 max_textures`, `u32 max_lights`, `u32 ring_bytes` | After `RESET`, `GET_INFO`, and once after the Zero boots. version = `0x00010000` for 1.0. |
| `0x03` | PONG | `u32 cookie` | `PING` |
| `0x04` | STATUS | `u32 frames`, `u32 crc_errors`, `u32 command_errors`, `u32 ring_free_bytes`, `u32 last_frame_us` | `GET_STATUS` |
| `0x11` | FRAME_DONE | `u32 frame_number`, `u32 render_us`, `u32 draws`, `u32 triangles` | After a frame whose `FRAME_END` had flag bit 0 set is handed to the panel |
| `0x7F` | ERROR | `u32 code`, `u32 opcode`, `u32 detail` | An invalid command (§6.4) or a CRC error (code 1, opcode 0) |

Error codes: 1 = CRC, 2 = unknown opcode, 3 = bad length, 4 = bad id,
5 = bad enum, 6 = no such object, 7 = out of memory, 8 = CLEAR after a draw,
9 = limit exceeded.

`ERROR` detail: the offending id for id and object errors, the received LENGTH
for length errors, the header word for CRC errors, otherwise 0.

The Pico can recognise a reboot of the Zero by an `INFO` reply it didn't ask
for; all objects and state are gone then.

### 9.1 Reply stream alignment

Reply packets are sent back to back or separated by idle words. A receiver
that doesn't track FS finds the packet boundaries in the bit stream: the first 1
bit after idle is bit 30 of a header (SYNC `0x5A` = `01011010`), so the header
starts one bit earlier. The packet is accepted if SYNC matches, LENGTH is within
limits and the CRC is correct; otherwise the search continues one bit after
that 1 bit. The next packet may follow directly after the CRC word.

---

## 10. Enumerations

### 10.1 Capabilities (ENABLE / DISABLE mask bits)

| Bit | Capability | Bit | Capability |
|---|---|---|---|
| 0 | DEPTH_TEST | 7 | LIGHT2 |
| 1 | CULL_FACE | 8 | LIGHT3 |
| 2 | BLEND | 9 | FOG |
| 3 | TEXTURE_2D | 10 | ALPHA_TEST |
| 4 | LIGHTING | 11 | COLOR_MATERIAL (vertex colour drives ambient and diffuse) |
| 5 | LIGHT0 | 12 | NORMALIZE |
| 6 | LIGHT1 | 13–31 | reserved, must be 0 |

### 10.2 Compare functions (depth, alpha)

0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOTEQUAL, 6 GEQUAL, 7 ALWAYS.
This is the same order as the V3D's depth-test field.

### 10.3 Blend factors

0 ZERO, 1 ONE, 2 SRC_COLOR, 3 ONE_MINUS_SRC_COLOR, 4 SRC_ALPHA,
5 ONE_MINUS_SRC_ALPHA, 6 DST_ALPHA, 7 ONE_MINUS_DST_ALPHA, 8 DST_COLOR,
9 ONE_MINUS_DST_COLOR, 10 SRC_ALPHA_SATURATE (source only).

### 10.4 Attributes

0 POSITION, 1 COLOR, 2 NORMAL, 3 TEXCOORD.

### 10.5 Array component types

| Value | Type | Notes |
|---|---|---|
| 0 | FLOAT | |
| 1 | SHORT | as is (not normalised); for POSITION and TEXCOORD |
| 2 | SHORT_NORM | normalised to −1 … 1 |
| 3 | UBYTE_NORM | normalised to 0 … 1; for COLOR with size 4 |
| 4 | BYTE_NORM | normalised to −1 … 1; for NORMAL |

### 10.6 Texture parameters

- Formats: 0 RGBA8888, 1 RGB565, 2 RGBA4444, 3 RGBA5551, 4 L8 (luminance),
  5 A8 (alpha), 6 LA88, 7 ETC1 (4 bits per pixel).
- Filters: 0 NEAREST, 1 LINEAR, 2 NEAREST_MIPMAP_NEAREST, 3 LINEAR_MIPMAP_NEAREST,
  4 NEAREST_MIPMAP_LINEAR, 5 LINEAR_MIPMAP_LINEAR. Mipmaps are reserved in v1:
  the mipmap filters behave like their base filter.
- Wrap modes: 0 REPEAT, 1 CLAMP_TO_EDGE, 2 MIRRORED_REPEAT.

### 10.7 Primitive modes and texture environment

- Primitive modes, the same values as GL: 0 POINTS, 1 LINES, 2 LINE_LOOP,
  3 LINE_STRIP, 4 TRIANGLES, 5 TRIANGLE_STRIP, 6 TRIANGLE_FAN.
- Texture environment modes: 0 MODULATE, 1 REPLACE, 2 DECAL, 3 BLEND.

---

## 11. Limits (v1)

| Limit | Value |
|---|---|
| buffers | 256 ids, total 16 MB |
| textures | 128 ids, 1 … 1024 × 1 … 1024, total 32 MB |
| lights | 4 |
| vertices per draw | 65536 |
| packet payload | 16384 words (64 KB) |
| command ring on the Zero | 1 MB |

The Pico should read the actual values from `INFO` rather than hard-code them.

---

## 12. Examples

All values are hexadecimal words in stream order. The CRC words were computed with
`zlib.crc32` over the little-endian bytes (§4.2).

`GET_INFO`:
```
A5020000 B5CEEAF9
```

`PING` with cookie `12345678`:
```
A5030001 12345678 19F2E571
```

`CLEAR` colour and depth, colour (R, G, B, A) = (0x10, 0x20, 0x40, 0xFF), depth 1.0:
```
A5100003 00000003 FF402010 3F800000 21BBBCEB
```

`DRAW_INLINE` of one triangle (TRIANGLES, 3 vertices, POSITION + COLOR),
red / green / blue corners:
```
A554000F
00000004 00000003 00000003
BF000000 BF000000 00000000 FF0000FF      (-0.5, -0.5, 0) red
3F000000 BF000000 00000000 FF00FF00      ( 0.5, -0.5, 0) green
00000000 3F000000 00000000 FFFF0000      ( 0.0,  0.5, 0) blue
1DBFBDD6
```

`FRAME_END` without a `FRAME_DONE` request:
```
A5110001 00000000 D9C51E2F
```

A complete first frame: `RESET`, `CLEAR`, `DRAW_INLINE`, `FRAME_END`, with idle
words wherever the Pico has nothing to send.

---

## 13. Pico library (*informative*)

The Pico side is a C library with a **GL ES 1.1-like API**. It encodes calls
into packets, computes the CRC with the DMA sniffer, and streams them over PIO,
checking READY between packets.

| Library call | Wire |
|---|---|
| `glClearColor`, `glClearDepthf`, `glClear` | `CLEAR` (sent before the first draw of the frame) |
| `glViewport`, `glDepthRangef` | `VIEWPORT` |
| `glMatrixMode`, `glLoadIdentity`, `glPushMatrix`, `glPopMatrix`, `glTranslatef`, `glRotatef`, `glScalef`, `glMultMatrixf`, `glOrthof`, `glFrustumf` | **kept in the library**; `LOAD_MATRIX` is sent when a changed matrix is needed by a draw |
| `glEnable`, `glDisable` | `ENABLE`, `DISABLE` (batched as masks) |
| `glLightfv`, `glMaterialfv`, `glLightModelfv`, `glFogf`, `glShadeModel` | `LIGHT`, `MATERIAL`, `LIGHT_MODEL`, `FOG`, `SHADE_MODEL` |
| `glColor4f`, `glNormal3f`, `glTexCoord2f` (as current values) | `COLOR`, `NORMAL`, `TEXCOORD` |
| `glGenBuffers`, `glBufferData`, `glBufferSubData`, `glDeleteBuffers` | `BUFFER_*` |
| `glGenTextures`, `glTexImage2D`, `glTexSubImage2D`, `glTexParameteri`, `glTexEnvi`, `glBindTexture` | `TEXTURE_*`, `TEX_ENV` |
| `glVertexPointer`, `glColorPointer`, `glNormalPointer`, `glTexCoordPointer` with a bound buffer, `glEnableClientState` | `ARRAY`, `ARRAYS_ENABLE` |
| `glDrawArrays`, `glDrawElements` with buffers | `DRAW_ARRAYS`, `DRAW_ELEMENTS` |
| `glDrawArrays` with **client-side arrays** (pointers into Pico RAM) | the library copies the vertices into `DRAW_INLINE` |
| end of frame (`eglSwapBuffers`) | `FRAME_END` |

Client-side arrays are supported by copying. That's the easy path for porting
existing code; uploading to buffers once is the fast path.

---

## 14. Zero implementation notes (*informative*)

- **Receive:** cyclic DMA from the PCM receive FIFO into a 1 MB ring buffer. The
  CPU parses packets from the ring and drives READY from its fill level.
- **Replies:** a second self-looping DMA control block plays a 1 MB transmit ring
  of idle words into the PCM transmit FIFO for as long as the Pico clocks. A reply
  is written about 1024 words ahead of the DMA's read position and zeroed again
  once the DMA has passed it.
- **Vertex processing on the ARM:** transform, GL ES 1.1 lighting (up to 4
  lights, no spot lights, infinite viewer), texture matrix, per-vertex fog,
  primitive assembly, clipping (near, far and a guard band), flat shading
  (last vertex), two-sided lighting (facing per triangle). Screen-space triangles
  go to the V3D in NV shader mode.
- **Guard band:** vertices stay within 800 px outside the viewport. Measured on
  the hardware at 320×240: up to x −960 … 1280 px renders, x −1120 … 1440 px
  loses triangles, although the 12.4 fixed-point format reaches ±2048.
- **Points and lines** are 1 pixel wide screen-space quads.
- **Fragment shaders:** 40 built-in QPU programs (`gpu/shaders.py`, generated with
  `devtools/qpuasm.py`): texture environment (none, MODULATE, REPLACE, DECAL,
  BLEND) × fog × alpha test × blending. Blending and the colour mask are done in
  the shader (tile buffer colour read); `SRC_ALPHA_SATURATE` is approximated by
  `SRC_ALPHA`.
- **Textures:** converted at upload to raster RGBA8888 (TMU type RGBA32R, R in
  byte 0), rows bottom-up as sent. The TMU reads raster rows at a stride of
  max(width, 4) texels (verified with widths 1, 2, 4, 8 and 64). ETC1 is decoded
  on the ARM. No mipmaps.
- **Output:** the V3D renders RGB565 directly into two alternating panel buffers.
  The ST7789 DMA driver sends them at 75 MHz (60 fps at 320×240).

## 15. Open points

- The exact READY thresholds and ring size, to be tuned against real workloads.
- Whether `DRAW_INLINE` should get 16-bit formats (half the bandwidth for
  screen-space 2D).
- Mipmaps (reserved in v1).
- Programmable pipeline (option B): opcodes `0x80`–`0xBF`, program blobs compiled
  offline (qpuasm now, later Mesa's `vc4` compiler via NIR on the PC).
