# Pico → RPi GPU command protocol, version 1

This document specifies the link between the **Pico 2 W** (the host, which runs
the application) and the **Raspberry Pi** (RPi: the GPU, which renders with the
VideoCore IV V3D and drives the ST7789 panel). Supported for now: the
Raspberry Pi Zero / Zero W and the Zero 2 W.

Version 1 has two pipelines, which can be mixed within a frame:
- a **fixed-function pipeline in the style of OpenGL ES 1.1** (option A), run by
  built-in QPU programs;
- **programs in the style of OpenGL ES 2.0** (option B, §7.10): GLSL ES 1.00
  vertex and fragment shaders, compiled on the PC by `tools/glslc` into QPU code
  and uploaded as program blobs. The RPi runs them in the V3D's GL shader mode,
  with vertex shading on the QPUs and clipping in hardware.

Status: **draft**. Everything is normative unless marked *informative*.

---

## 1. Layers

| Layer | What it defines |
|---|---|
| Physical | pins, I2S timing, the READY and FRAME signals (§2, §3) |
| Packet | framing of 32-bit words: header, payload, CRC32 (§4) |
| Command | execution model, opcodes, payloads, replies and enums (§6–§10) |
| API (Pico library) | the OpenGL ES 2.0 and 1.1 API, encoded into commands (§13, informative) |

---

## 2. Physical layer

### 2.1 Pins

| Signal | Direction | Pico 2 W pin (GPIO) | RPi pin (GPIO, function) |
|---|---|---|---|
| DATA | Pico → RPi | 21 (GP16) | 38 (GPIO20, PCM_DIN) |
| BCLK | Pico → RPi | 22 (GP17) | 12 (GPIO18, PCM_CLK) |
| FS | Pico → RPi | 24 (GP18) | 35 (GPIO19, PCM_FS) |
| REPLY | RPi → Pico | 25 (GP19) | 40 (GPIO21, PCM_DOUT) |
| READY | RPi → Pico | 26 (GP20) | 36 (GPIO16) |
| FRAME | RPi → Pico | 27 (GP21) | 37 (GPIO26) |
| GND | — | 23, 28 | 39, 34 |

READY has an external 10 kΩ pull-down to GND on the Pico side (see §3.1).
All signals are 3.3 V push-pull.

### 2.2 I2S format

The Pico is the I2S **master**: it drives BCLK and FS. The RPi's PCM block is
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
  idle words (§4.4) when it has nothing to send. This is required: the RPi can
  only reply (REPLY, §9) while the Pico clocks, and a steady clock keeps the
  RPi's receiver in frame lock.

### 2.3 Direction of the data lines

- DATA (GP16 → PCM_DIN) carries the **command stream**.
- REPLY (PCM_DOUT → GP19) carries the **reply stream**, clocked by the same
  BCLK/FS. The RPi changes REPLY on the falling edge of BCLK.
- *Informative (v1 Pico library):* a second PIO state machine samples GP19 once per
  bit, in lockstep with the one that drives BCLK (same clock divider, started in
  the same cycle, input synchroniser bypassed). It doesn't track FS, so the reply
  words arrive with an unknown bit offset. The parser finds each packet's
  alignment from its header (§9.1). Both sampling phases (at the rising and at the
  falling BCLK edge) were measured error-free at 75 MHz.

---

## 3. Side-band signals

### 3.1 READY (RPi → Pico): flow control

- READY **high** means the RPi's command ring buffer has room for at least
  **one maximum-size packet** (§4.3) plus margin. The Pico may start sending a packet.
- READY **low** means the Pico must not **start** a new packet. A packet already
  started is always completed; the READY rule guarantees there's room for it.
- The Pico samples READY **before sending each packet header**.
- Hysteresis (*informative*, v1 implementation): the RPi drops READY when free
  space falls below 128 KB and raises it again at 256 KB free, out of a 1 MB ring.
- **Reset safety:** while the RPi boots or reboots, GPIO16 is an input with a
  pull-down. READY then reads **low**, so the Pico never streams into an RPi that
  isn't running. The external 10 kΩ pull-down covers an RP2350 GPIO pull-down
  erratum (E9), where an undriven pin can float high with only the internal pull-down.
- After the RPi comes up, the Pico must send `RESET` (§7.1) before anything else.

### 3.2 FRAME (RPi → Pico): frame pacing

The RPi drives FRAME high for at least 10 µs when a frame starts its transfer to
the panel (the moment `FRAME_END`'s image is handed to the panel DMA). The Pico
can use a GPIO edge interrupt to pace its main loop without decoding REPLY.
FRAME is optional for the Pico to use; the RPi always drives it.

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
| 31–24 | SYNC | `0xA5` for commands (Pico → RPi), `0x5A` for replies (RPi → Pico) |
| 23–16 | OPCODE | command or reply code (§6, §9) |
| 15–0 | LENGTH | payload length in 32-bit words |

### 4.2 CRC32

- The algorithm is CRC-32 as used by zlib and Ethernet: reflected polynomial
  `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`.
- It covers the header and payload words, each taken as **4 little-endian bytes**.
  In C, that's the words exactly as they lie in memory on either processor.
- *Informative:* the RP2350's DMA sniffer computes this CRC in hardware while the
  DMA feeds the PIO, so it costs the Pico nothing. On the RPi, a table-driven
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
  in the middle of a packet. The RPi also reports the error (`ERROR`, §9).
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

- Commands between two `FRAME_END` packets form a **frame**. The RPi executes
  commands in stream order. It collects the frame's draws, bins and renders them
  when `FRAME_END` arrives, and hands the image to the panel.
- **State persists across frames**: matrices, enables, bound textures, arrays and
  objects stay as set until changed.
- The Pico may start sending the next frame immediately; the ring buffer absorbs
  it. Back-pressure comes only from READY.

### 6.2 Render targets, jobs and clearing

- Draws go to the bound **render target**: the panel (framebuffer 0) or a
  texture (§7.2, `BIND_FRAMEBUFFER`). The RPi collects the draws for the bound
  target and renders them as one V3D job when the target changes, when the
  frame ends, or when pixels are read (`READ_PIXELS`, `COPY_TEX_IMAGE`).
- **`CLEAR` works like `glClear`, at any time.** It respects the scissor
  rectangle, the colour mask, the depth mask and the stencil write mask. At the
  start of a job (no draws yet) without scissor or masks it is the tile
  buffer's clear, which is free; otherwise it is drawn as a rectangle over the
  target.
- If the panel gets no colour `CLEAR` in a frame, it starts from the previous
  frame's image; without a depth or stencil `CLEAR`, from the previous depth and
  stencil. A texture target keeps its contents, and its depth and stencil, in the
  same way. Keeping contents costs a load of the tiles from memory.

### 6.3 Object updates within a frame

- `BUFFER_DATA` and `TEXTURE_DATA` take effect for **draws issued after them**,
  with GL semantics. If the object was already used by an earlier draw in the
  same frame, the RPi keeps the old contents for those draws (copy-on-write).
- Deleting an object used by the current frame is allowed. The RPi frees it
  once the frame has rendered.
- *Informative:* frequently changing geometry is best sent with `DRAW_INLINE`.

### 6.4 The screen: panel or HDMI

- Framebuffer 0 ("the panel" elsewhere in this document) is the **screen**: the
  ST7789 panel (320×240) or HDMI, where it is a framebuffer of a size chosen
  for the monitor, which the RPi's firmware scales to the HDMI mode.
- By default the screen is on HDMI while a monitor is connected and on the
  panel otherwise; the kernel command line can change that (§14). The RPi
  watches the HDMI hot-plug line and reads a new monitor's EDID.
- **The screen changes only between frames**: before the first draw of a
  frame. Both of its images are cleared then (a frame without a colour `CLEAR`
  starts from black), and its depth and stencil are undefined until cleared.
  The GL state does not change: the viewport and scissor box keep their values
  (pgl moves ones that covered the whole screen, §13.1).
- After each change the RPi sends `DISPLAY` (§9), and `INFO` gives the new size.

### 6.5 Errors

- An invalid command is ignored as a whole and reported with an `ERROR` reply.
  Invalid means an unknown opcode, a wrong LENGTH, an out-of-range id or enum, or
  a missing object.
- Errors never stop the stream.

---

## 7. Commands (Pico → RPi, SYNC `0xA5`)

Payload fields are listed word by word. `[n]` means n words.

### 7.1 System

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x01` | RESET | — | Delete all objects and reset all state to the defaults (§7.11). Discard the current frame. Replies `INFO`. |
| `0x02` | GET_INFO | — | Replies `INFO` (§9). |
| `0x03` | PING | `u32 cookie` | Replies `PONG` with the same cookie, once all earlier commands have been **parsed**. |
| `0x04` | GET_STATUS | — | Replies `STATUS` (§9). |

### 7.2 Frame

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x10` | CLEAR | `u32 mask`, `color color`, `f32 depth` [, `u32 stencil`] | Clear the bound target (§6.2). mask bit 0 = colour, bit 1 = depth, bit 2 = stencil. |
| `0x11` | FRAME_END | `u32 flags` | End the frame: render and present it. flags: bit 0 = request a `FRAME_DONE` reply. |
| `0x12` | VIEWPORT | `s32 x`, `s32 y`, `u32 width`, `u32 height`, `f32 near`, `f32 far` | Like `glViewport` + `glDepthRangef`. |
| `0x13` | FRAMEBUFFER_CREATE | `id framebuffer`, `u32 texture` (id \| cube face << 24), `u32 flags` | A render target: level 0 of the texture, and with flags bit 0 a depth and stencil buffer: its own, or with flags bits 15–8 = n (1–16) buffer n, shared by the framebuffers that name it. A texture without alpha (RGB, RGB565, L, ETC1) is a target without alpha: `DST_ALPHA` reads 1, and sampling it reads alpha 1. Recreating an id replaces it. |
| `0x14` | FRAMEBUFFER_DELETE | `id framebuffer` | Delete it; the panel is bound if it was bound. |
| `0x15` | BIND_FRAMEBUFFER | `id framebuffer` | Draw to this target; 0 = the panel. Renders what the old target collected. |
| `0x16` | READ_PIXELS | `s32 x`, `s32 y`, `u32 width`, `u32 height` | Like `glReadPixels` (RGBA, unsigned bytes) from the bound target: `PIXELS` replies (§9). At most 262144 pixels. |
| `0x17` | COPY_TEX_IMAGE | `u32 texture` (id \| level << 16 \| face << 24), `u16 xoffset, u16 yoffset`, `s32 x`, `s32 y`, `u32 width`, `u32 height` | Like `glCopyTexSubImage2D`: pixels of the bound target into a texture level. |

### 7.3 Buffers (vertex and index data)

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x20` | BUFFER_CREATE | `id buffer`, `u32 size_bytes` | Create a buffer (contents undefined). Recreating an existing id replaces it. |
| `0x21` | BUFFER_DATA | `id buffer`, `u32 offset_bytes`, `u32 length_bytes`, `bytes data[…]` | Write data (any offset and length). |
| `0x22` | BUFFER_DELETE | `id buffer` | Delete it. |

### 7.4 Textures

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x28` | TEXTURE_CREATE | `id texture`, `u16 width, u16 height`, `u32 format` | Create a texture, 1 … 2048 × 1 … 2048. format bit 8: a cube map (square; faces +X, −X, +Y, −Y, +Z, −Z). Power-of-two textures have a full mip chain, others only level 0. |
| `0x29` | TEXTURE_DATA | `u32 texture` (id \| level << 16 \| face << 24), `u16 x, u16 y`, `u16 width, u16 height`, `bytes pixels[…]` | Upload a rectangle of a level, rows bottom-up (GL order), tightly packed, each row padded to 4 bytes. It defines the level (as `glTexImage2D`); a level may be sent in several parts. ETC1: x and y are multiples of 4 and the data is ETC1 blocks. |
| `0x2A` | TEXTURE_PARAMS | `id texture`, `u32 min_filter`, `u32 mag_filter`, `u32 wrap_s`, `u32 wrap_t` | Filtering and wrapping (§10.6). |
| `0x2B` | TEXTURE_DELETE | `id texture` | Delete it. |
| `0x2C` | TEXTURE_BIND | `id texture` | Bind to unit 0 (0 = none). |
| `0x2D` | TEX_ENV | `u32 mode`, `color env_color` | Texture environment: MODULATE, REPLACE, DECAL or BLEND (§10.7). |
| `0x2E` | GENERATE_MIPMAP | `id texture` | Like `glGenerateMipmap` (box filter). |

A texture that isn't complete by GL ES 2.0 rules (§3.8.2: every level a
mipmapping filter needs, and for non-power-of-two sizes `CLAMP_TO_EDGE` and no
mipmapping) samples as (0, 0, 0, 1) in programs and disables texturing in the
fixed-function pipeline. The default minification filter is
`NEAREST_MIPMAP_LINEAR`, as in GL.

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
| `0x3C` | BLEND_FUNC_SEPARATE | `u32 src_rgb`, `u32 dst_rgb`, `u32 src_alpha`, `u32 dst_alpha` | Like `glBlendFuncSeparate` (`BLEND_FUNC` sets both). |
| `0x3D` | BLEND_EQUATION | `u32 mode_rgb`, `u32 mode_alpha` | ADD, SUBTRACT, REVERSE_SUBTRACT (§10.3). |
| `0x3E` | BLEND_COLOR | `f32 r, g, b, a` | The constant colour of the CONSTANT_* factors. |
| `0x60` | STENCIL_FUNC | `u32 face`, `u32 func`, `u32 ref`, `u32 mask` | Like `glStencilFuncSeparate` (face: FRONT, BACK, FRONT_AND_BACK). |
| `0x61` | STENCIL_OP | `u32 face`, `u32 fail`, `u32 zfail`, `u32 zpass` | Like `glStencilOpSeparate` (§10.3). |
| `0x62` | STENCIL_MASK | `u32 face`, `u32 mask` | Like `glStencilMaskSeparate`. |
| `0x39` | SCISSOR | `s32 x`, `s32 y`, `u32 width`, `u32 height` | Like `glScissor` (window coordinates, origin bottom left); applies with `SCISSOR_TEST` enabled. |
| `0x3A` | POLYGON_OFFSET | `f32 factor`, `f32 units` | Like `glPolygonOffset`; applies to triangles with `POLYGON_OFFSET_FILL` enabled. |
| `0x3B` | LINE_WIDTH | `f32 width` | Like `glLineWidth`: 1 … 32 pixels (clamped). |

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

### 7.10 Programs (GL ES 2.0 subset)

A **program** is a vertex shader and a fragment shader, precompiled on the PC by
`tools/glslc/glslc.py` (GLSL ES 1.00, through Mesa's `vc4` compiler). The blob
(`protocol/pgpu_program.h`) holds QPU code for the primitive classes chosen when
compiling (triangles, lines, points), each with two fragment shader endings: a
plain one, and one that blends and masks from uniforms, so every blend state
and colour mask works with every program. The compiler also writes a C header
with the blob, the attribute locations, the uniform storage offsets and the
sampler indices, and the names and GL types that `glProgramBinaryOES` needs (§13).

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0x80` | PROGRAM_CREATE | `id program`, `u32 size_words` | Create a program of this blob size. Recreating an existing id replaces it. |
| `0x81` | PROGRAM_DATA | `id program`, `u32 offset_words`, `words blob[…]` | Store part of the blob. When the last word has arrived, the RPi checks the blob and loads the code; an invalid blob is reported as `ERROR` 10 and the program stays unusable. |
| `0x82` | PROGRAM_DELETE | `id program` | Delete it. |
| `0x83` | USE_PROGRAM | `id program` | Draw with this program; 0 = the fixed-function pipeline. |
| `0x84` | PROGRAM_UNIFORM | `id program`, `u32 offset_words`, `words values[…]` | Write the program's uniform storage (offsets from the compiler's header). Values as Mesa's `vc4` driver stores them (it has native integers): `float` uniforms as 32-bit floats, `int` uniforms as 32-bit integers, `bool` uniforms as 0 (false) or `0xFFFFFFFF` (true). |
| `0x85` | PROGRAM_SAMPLER | `id program`, `u32 sampler`, `u32 unit` | Like `glUniform1i` on a sampler: the texture unit (0–7) a sampler reads. Default: sampler n reads unit n. |
| `0x86` | TEXTURE_BIND_UNIT | `u32 unit`, `id texture` | Bind a texture to unit 0–7 (0 = none). `TEXTURE_BIND` is unit 0. |
| `0x87` | VERTEX_ATTRIB | `u32 index`, `f32 v[4]` | Current value of a generic attribute (0–7), used when its array is disabled. Default (0, 0, 0, 1). |
| `0x88` | ATTRIB_ARRAY | `u32 index`, `id buffer`, `u32 offset_bytes`, `u32 stride_bytes`, `u32 size`, `u32 type` | Point generic attribute 0–7 at a buffer. stride 0 = tightly packed (over 255 bytes, the RPi copies the vertices used). |
| `0x89` | ATTRIBS_ENABLE | `u32 mask` | Which generic attributes come from arrays (bit n = attribute n). |
| `0x8A` | PROGRAM_DRAW_INLINE | `u32 mode`, `u32 vertices`, `u32 attribute_mask`, `u32 index_count`, `u32 index_type`, then per attribute in the mask (ascending): `u32 format`, `bytes data[…]`; then `bytes indices[…]` | Draw with vertex data carried in the packet (client-side arrays). format = type \| size << 8; other formats than the program's are converted. The data is `vertices` values, tightly packed and padded to a word. index_count 0 = draw the vertices in order; otherwise index_count indices follow, u8 or u16. Attributes not in the mask come from `ATTRIB_ARRAY` (first vertex 0) or `VERTEX_ATTRIB`. |

**Drawing:** while a program is in use, `DRAW_ARRAYS` and `DRAW_ELEMENTS` use
it and the generic attributes, and `PROGRAM_DRAW_INLINE` draws vertex data from
the packet. `DRAW_INLINE` (the fixed-function layout) isn't available with
programs (error 5). The RPi picks the variant for the primitive mode; if the
program wasn't compiled for it, the draw is reported as `ERROR` 10.

- **Attributes:** arrays of any type and size can be used. If they differ from
  what the program was compiled for (the fast path), the RPi converts the
  vertices used (missing components (0, 0, 0, 1), as GL). A disabled array uses
  the current value from `VERTEX_ATTRIB`.
- **State that applies:** viewport and depth range, depth test and mask, culling
  and front face, blending (all of §10.3), colour mask, stencil, textures and
  their parameters, scissor, polygon offset and line width.
  **Ignored:** lighting, fog, alpha test, texture environment, shade model and the
  fixed-function matrices.
- **Textures:** 2D and cube-map samplers; formats as GL ES 2.0 (`A8` reads
  (0, 0, 0, A), `L8` (L, L, L, 1)). A sampler whose unit has no complete
  texture reads (0, 0, 0, 1).
- **GLSL:** GLSL ES 1.00 as compiled by Mesa's `vc4` driver: `gl_FragCoord`,
  `gl_FrontFacing`, `gl_PointCoord` and `gl_DepthRange` follow GL on the panel
  and on texture targets (verified numerically in self test 8). Mesa compiles
  the origin of `gl_PointCoord` into the fragment shader, so glslc adds a
  points variant for texture targets (`PGPU_VK_TEXTURE_TARGET`), which the RPi
  uses when drawing points into a texture. The panel has no alpha channel (`DST_ALPHA`
  reads 1); texture targets have one. Control flow (loops, `break`, `discard`,
  branches that differ per pixel) and dynamic indexing of uniform arrays work
  (verified in self test 8); the latter reads the program's uniform storage
  through the TMU (uniform kind 11, a copy of the storage per draw).
  `gl_FragCoord` is at pixel centres (x + 0.5, y + 0.5): Mesa's `vc4` returns
  the integer pixel coordinates, which `patches/mesa-vc4-dump.patch` fixes.
- **Limits:** 64 programs, blob at most 65536 words, uniform storage at most 4096
  words, 8 attributes, 8 samplers, `count` at most 65535 per draw.
- `DRAW_ARRAYS` with a program reads the arrays starting at `first`: `first` can
  be any value, the array just has to hold the vertices.

### 7.11 Default state (after power-up and `RESET`)

| State | Default |
|---|---|
| matrices | identity |
| viewport | the whole screen (0, 0, width, height), depth range 0 … 1 |
| enables | all off, except DITHER |
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
| bound texture | none (all units) |
| program | 0 (fixed function); no programs |
| framebuffer | 0 (the panel); no framebuffers |
| blending | ONE, ZERO for RGB and alpha; ADD; blend colour (0, 0, 0, 0) |
| stencil | test off; func ALWAYS, ref 0, masks 0xFF; ops KEEP (both faces) |
| scissor | disabled, (0, 0, width, height) |
| polygon offset | disabled, factor 0, units 0 |
| line width | 1 |
| generic attributes | arrays disabled, current values (0, 0, 0, 1) |

### 7.12 Video

Video streams are decoded by the RPi's VideoCore into **video textures**:
ordinary 2D textures (sampled by any program or the fixed function, on any
geometry) whose pixels are the frame of the stream that's due. The host sends
the compressed samples, with their presentation times; the RPi decodes,
scales and converts them in the VideoCore (its H.264 decoder and ISP), keeps
the stream's clock and, at the end of each frame (`FRAME_END`), gives the
texture the newest decoded frame whose time has come. So every draw of a
frame sees the same video frame, and the video's frame rate is independent of
the GL frame rate.

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0xC0` | VIDEO_OPEN | `u32 stream` (1 or 2), `u32 codec` (1 = H.264), `id texture`, `u16 width, u16 height`, `u16 coded_width, u16 coded_height`, `u32 format`, `u32 config_bytes`, `u8 config[config_bytes]` (padded to words) | The texture becomes (again) a video texture: RGBA, width × height (width a power of two, 32 … 2048; height a multiple of 16), one level, clamped (repeat and mirror work at power-of-two sizes), linear; black until the first frame. The stream is (re)opened for H.264 of the coded size, scaled to the texture (the whole picture: aspect is the drawing's business). Format 0 Annex B: samples with start codes, SPS and PPS in the stream (a CONFIG sample); format 1 AVCC: samples as MP4 stores them (NAL units with length prefixes), config the MP4's avcC (SPS and PPS, the length size). An open stream is closed first. |
| `0xC1` | VIDEO_DATA | `u32 stream`, `u32 flags`, `s64 pts` (2 words, low first; microseconds), `u32 bytes` (in this packet), `u32 sample_bytes` (the whole sample's), `u8 data[bytes]` (padded to words) | A chunk of a **sample**: one access unit in decode order, in the stream's format. Flags: bit 0 FIRST chunk, bit 1 LAST chunk, bit 2 KEYFRAME, bit 3 CONFIG (SPS and PPS), bit 4 EOS (the stream's end; may have no data). A sample's chunks come in order, nothing of the stream between them; the pts and `sample_bytes` of the FIRST chunk count. A sample is taken whole or not at all (`LIMIT` for a sample that doesn't fit: its other chunks are then ignored). |
| `0xC2` | VIDEO_CONTROL | `u32 stream`, `u32 op`, `s64 arg` | Op 1 PLAY: the clock runs from `arg` now (`0x8000000000000000`: from where it stands). Op 2 PAUSE: the clock stops (the texture keeps its frame). Op 3 CLOSE: the stream ends, the texture stays, black. Op 4 RESIZE: the texture becomes `arg` bits 15:0 wide, 31:16 high (the rules of `VIDEO_OPEN`), the stream goes on: the frames decoded and waiting are dropped, the ISP scales to the new size from the next one (for a screen that changed: the video keeps its place). |
| `0xC3` | VIDEO_GET_STATUS | `u32 stream` | Replies `VIDEO_STATUS` (§9). |

- **The clock:** without `PLAY` it starts when the first frame is shown, from
  that frame's pts. Frames whose time has passed are dropped for the newest
  due one; frames before their time wait (8 at most; then the decoder waits).
  If the next frame lies more than a second ahead of the clock and of the
  frame on screen (a gap: samples the host skipped or lost), the clock jumps
  to it, so the video never stops for a gap.
- **Flow control:** the RPi holds 4 MB and 256 samples of a stream not yet
  taken by its decoder. `VIDEO_STATUS` (every 100 ms while the stream is open,
  and on request) says how many bytes and samples the decoder has taken since
  the open; the host sends a sample only if it fits in what's left
  (`pgpu_video_room`, §13). The video's data then never holds up the GL
  commands behind it.
- **Looping and seeking:** a stream is one clock: to loop, send the samples
  again with the times going on (the file's duration added); to jump, close
  and open again. To change the texture's size, `RESIZE` (not a new open: that
  would start the stream again).
- Each stream has its own decoder and ISP in the VideoCore; the two streams
  can play at once (720p and smaller; one 1080p30 stream has headroom,
  measured: 43 fps).

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
| `0x80`–`0x8F` | programs (§7.10) |
| `0x90`–`0xBF` | reserved for program additions |
| `0xC0`–`0xC7` | video (§7.12) |
| `0xC8`–`0xEF` | reserved |
| `0xF0`–`0xFF` | debug and vendor |

Debug commands (v1 RPi implementation, not needed by applications):

| Op | Name | Payload | Meaning |
|---|---|---|---|
| `0xF0` | DEBUG_SCREENSHOT | — | The RPi writes the last presented frame to its USB development log (base64 RGB565 between `#SCREENSHOT` and `#END` lines; `devtools/screenshot.py` makes a PNG). |

---

## 9. Replies (RPi → Pico, SYNC `0x5A`)

Replies use the same packet format, sent on REPLY. Idle words are `0x00000000`.
A reply to a request uses the request's opcode (`GET_INFO` → `INFO`, `PING` → `PONG`, …).

| Op | Name | Payload | Sent when |
|---|---|---|---|
| `0x02` | INFO | `u32 version`, `u16 width, u16 height` (the screen's, §6.4), `u32 max_texture_size`, `u32 max_buffers`, `u32 max_textures`, `u32 max_lights`, `u32 ring_bytes` | After `RESET`, `GET_INFO`, and once after the RPi boots. version = `0x00010000` for 1.0. |
| `0x03` | PONG | `u32 cookie` | `PING` |
| `0x04` | STATUS | `u32 frames`, `u32 crc_errors`, `u32 command_errors`, `u32 ring_free_bytes`, `u32 last_frame_us`, then the last measuring window (about a second): `u32 window_us`, `u32 window_frames`, `u32 v3d_busy_us` (binning and rendering), `u32 arm_busy_us` (receiving and executing commands, without the waits for the V3D and the panel), `u32 panel_wait_us` (for the panel DMA of the previous frame). Older Zeros send the first 5 words | `GET_STATUS` |
| `0x05` | DISPLAY | `u32 output_flags`, `u16 width, u16 height`, `u16 monitor_width, u16 monitor_height`, `u32 monitor_refresh_mhz`, `u16 signal_width, u16 signal_height`, `u8 monitor_name[16]` (see below) | After each `INFO`, and whenever the screen or the HDMI monitor changes (§6.4) |
| `0x06` | VIDEO_STATUS | `u32 stream`, `u32 flags` (bit 0 open, bit 1 playing, bit 2 ended: the EOS came out of the decoder, bit 3 error: a VideoCore component reported one), `u32 bytes_done`, `u32 ring_bytes`, `u32 decoded`, `u32 shown`, `u32 dropped` (frames), `s64 shown_pts` (2 words; `0x8000000000000000`: none), `u32 waiting` (decoded frames before their time), `u32 samples_done`, `u32 max_samples` | Every 100 ms while a stream is open, and `VIDEO_GET_STATUS`. `bytes_done` and `samples_done` count what the decoder has taken since `VIDEO_OPEN` (mod 2^32): the host may have `ring_bytes` and `max_samples` more in flight (§7.12) |
| `0x11` | FRAME_DONE | `u32 frame_number`, `u32 render_us`, `u32 draws`, `u32 triangles` | After a frame whose `FRAME_END` had flag bit 0 set is handed to the panel |
| `0x16` | PIXELS | `u32 offset`, `color pixels[1 … 61]` | `READ_PIXELS`: the pixels from `offset` (in pixels, rows bottom up), in as many replies as needed |
| `0x7E` | CREDIT | `u32 bytes` | USB stream only (§13.3): the stream bytes the RPi has taken so far, since the session started |
| `0x7F` | ERROR | `u32 code`, `u32 opcode`, `u32 detail` | An invalid command (§6.5) or a CRC error (code 1, opcode 0) |

Error codes: 1 = CRC, 2 = unknown opcode, 3 = bad length, 4 = bad id,
5 = bad enum, 6 = no such object, 7 = out of memory, 8 = (not used any more),
9 = limit exceeded, 10 = program (invalid blob or no variant for the primitive).

`ERROR` detail: the offending id for id and object errors, the received LENGTH
for length errors, the header word for CRC errors, otherwise 0.

The Pico can recognise a reboot of the RPi by an `INFO` reply it didn't ask
for; all objects and state are gone then.

`DISPLAY` fields:

- `output_flags`: bits 0–7 the screen's output (1 = the panel, 2 = HDMI); bit 8:
  an HDMI monitor is connected; bit 9: a panel is configured (the RPi can't
  detect one); bit 10: the monitor's EDID was read.
- `width`, `height`: the screen (framebuffer 0), as in `INFO`.
- `monitor_width`, `monitor_height`, `monitor_refresh_mhz`: the monitor's
  preferred mode from its EDID (refresh in millihertz); 0 without an EDID.
- `signal_width`, `signal_height`: the mode the RPi sends on HDMI. The
  firmware sets it at boot (`config.txt`, §14) and keeps it.
- `monitor_name`: the monitor's name from the EDID (up to 13 characters), zero
  padded; empty if it has none.

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
| 6 | LIGHT1 | 13 | SCISSOR_TEST |
| | | 14 | POLYGON_OFFSET_FILL |
| | | 15 | STENCIL_TEST |
| | | 16 | DITHER (the panel's RGB565 output is dithered; enabled by default, as in GL; applies to a whole job, with the state when it renders) |
| | | 17–31 | reserved, must be 0 |

### 10.2 Compare functions (depth, alpha)

0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOTEQUAL, 6 GEQUAL, 7 ALWAYS.
This is the same order as the V3D's depth-test field.

### 10.3 Blend factors

0 ZERO, 1 ONE, 2 SRC_COLOR, 3 ONE_MINUS_SRC_COLOR, 4 SRC_ALPHA,
5 ONE_MINUS_SRC_ALPHA, 6 DST_ALPHA, 7 ONE_MINUS_DST_ALPHA, 8 DST_COLOR,
9 ONE_MINUS_DST_COLOR, 10 SRC_ALPHA_SATURATE (source only), 11 CONSTANT_COLOR,
12 ONE_MINUS_CONSTANT_COLOR, 13 CONSTANT_ALPHA, 14 ONE_MINUS_CONSTANT_ALPHA.

Blend equations: 0 ADD, 1 SUBTRACT, 2 REVERSE_SUBTRACT.

Stencil operations: 0 KEEP, 1 ZERO, 2 REPLACE, 3 INCR, 4 DECR, 5 INVERT,
6 INCR_WRAP, 7 DECR_WRAP.

In the fixed-function pipeline SRC_ALPHA_SATURATE is approximated by SRC_ALPHA.

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
| 5 | UBYTE | as is (0 … 255) |
| 6 | BYTE | as is (−128 … 127) |
| 7 | USHORT | as is |
| 8 | USHORT_NORM | normalised to 0 … 1 |
| 9 | FIXED | 16.16 fixed point |

### 10.6 Texture parameters

- Formats: 0 RGBA8888, 1 RGB565, 2 RGBA4444, 3 RGBA5551, 4 L8 (luminance),
  5 A8 (alpha), 6 LA88, 7 ETC1 (4 bits per pixel), 8 RGB888.
- Filters: 0 NEAREST, 1 LINEAR, 2 NEAREST_MIPMAP_NEAREST, 3 LINEAR_MIPMAP_NEAREST,
  4 NEAREST_MIPMAP_LINEAR, 5 LINEAR_MIPMAP_LINEAR.
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
| textures | 128 ids, 1 … 2048 × 1 … 2048, total 32 MB |
| framebuffers | 16 ids |
| texture units | 8 |
| lights | 4 |
| vertices per draw | 65536 |
| packet payload | 16384 words (64 KB) |
| command ring on the RPi | 1 MB |
| video streams | 2; a stream: 4 MB and 256 samples not yet decoded, 8 decoded frames waiting |
| video textures | width a power of two, 32 … 2048; height a multiple of 16, … 2048 |

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

The host library (`libpgpu/`, board independent) has two layers:

- **`pgpu`** (`libpgpu/pgpu.{h,c}`): one C function per command. It
  batches packets, splits large uploads, turns client-side arrays into
  `PROGRAM_DRAW_INLINE`, and parses replies into queues (`ERROR` replies
  separately, `pgpu_poll_error`), including the reply stream of an I2S link
  (`pgpu_rx_parse`, §9.1). The words go through a transport
  (`libpgpu/pgpu_link.h`, one per link in `transports/`): on the Pico the I2S
  link (`transports/pico-i2s`: DMA over PIO, READY before each batch, replies
  sampled into a DMA ring and parsed in a 1 ms timer); on a PC the RPi's USB
  (`transports/pc-usb`, §13.3). The builds per host board are in `hosts/`.
- **`pgl`** (`libpgpu/gles/pgl.{h,c}`): the **OpenGL ES 2.0 API**, with
  the **GL ES 1.1 fixed-function calls** for program 0. It keeps the GL state
  (for `glGet*`, `glIsEnabled`, object names) and encodes it into commands.
  `gltest.c` (self test 8) exercises it using only `gl*` calls, on the Pico and
  on a PC.
- **Video** (§7.12): `pglVideoTexture (texture, stream, width, height,
  coded_width, coded_height, avcc, avcc_bytes)` makes a GL texture name a
  video texture (`VIDEO_OPEN`; pgl then knows it as a complete RGBA texture,
  linear, clamped); `pglVideoResize (texture, stream, width, height)` changes
  its size, the stream going on (`RESIZE`). `pgpu_video_room` says how big a sample may be now (from
  the last `VIDEO_STATUS`, which the library keeps per stream like `DISPLAY`,
  and what it has sent since); `pgpu_video_control` and
  `pgpu_video_get_status` the rest.
- **The data path from a file:** a file is read through a callback
  (`pgpu_read_t`: bytes at an offset), so it needn't be in memory: on an SD
  card the filesystem reads its sectors as needed.
  - `libpgpu/pgpu_mp4.{h,c}` reads an MP4's H.264 track through it: the boxes
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

### 13.1 How pgl maps GL to the wire

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

### 13.2 Differences from GL ES 2.0 and 1.1

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
  no effect on them. On a PC, pgl compiles (§13.3).
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

### 13.3 pgl on a PC (tests)

pgl and pgpu also build for Linux (`hosts/pc`): the transport
(`transports/pc-usb/pgpu_host.c`) sends the packets to the RPi over its USB serial link (the
gpu app's devlink), so programs on the PC drive the GPU without the Pico:

- **The stream:** the PC sends `piegpu-stream` (a new session each time);
  the RPi answers `#STREAM`, takes all following bytes as command packets
  (§4, byte aligned) and ignores the I2S input from then on. Replies come back
  on the same link; the log goes on as text, which the PC skips (packets are
  found by their header and CRC).
- **Flow control:** the RPi's USB gadget buffers 64 KB and drops what doesn't
  fit, so the RPi sends `CREDIT` replies (§9) with the number of stream bytes
  taken; the PC keeps at most 6 KB unacknowledged (`PGPU_WINDOW` sets it).
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

## 14. RPi implementation notes (*informative*)

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
- **Points and lines** are screen-space quads, 1 pixel and `LINE_WIDTH` wide.
- **Jobs:** each render target's draws are one V3D job (binning, then rendering
  64×64 tiles). Colour is loaded from memory unless cleared; depth and stencil
  are stored after every job (T-format, about 80 µs at 320×240) and loaded
  unless cleared. A frame that switches targets renders several jobs.
- **Texture targets** are level 0 of a texture, in its tiled layout (T or LT
  RGBA8888, the tile buffer's format), not y-flipped: rows in GL order.
- **Stencil:** every fragment shader writes the three TLB stencil setup words
  from uniforms (Mesa's encoding); with the test off they say "always pass, keep".
- **Scissor** and the viewport become the V3D's clip window; polygon offset is
  its depth offset (factor and units as in Mesa's `vc4`). An empty clip window
  doesn't clip everything on the V3D: such draws are skipped.
- **V3D state that persists across jobs:** the line width keeps its value from
  the previous job (measured: lines drew wrongly after wide-line tests), so
  each job emits it before first use; the depth offset is treated the same.
- **Programs** run in the V3D's GL shader mode: the binner runs the coordinate
  shader, the renderer the vertex and fragment shaders, and the hardware clips.
  Each draw gets a shader record, attribute records pointing straight into the
  buffers, and uniform streams resolved at draw time. Buffers used by a program
  draw are copied on write for the rest of the frame (§6.3). The viewport has a
  negative y scale on the panel, because its rows go top to bottom.
- **Program fragment shaders** are compiled by Mesa for an RGBA8888 window
  framebuffer with blending off; `glslc` replaces the final colour write with one
  of two endings (`devtools/qpuasm.py`): reorder Mesa's BGRA into the tile
  buffer's RGBA (8 instructions), or load the tile buffer's colour and blend
  per channel with 48 coefficient uniforms, which cover every blend factor and
  equation, the constant colour and the colour mask (about 75 instructions).
- **Fixed-function fragment shaders:** 40 built-in QPU programs (`gpu/shaders.py`,
  generated with `devtools/qpuasm.py`): texture environment (none, MODULATE,
  REPLACE, DECAL, BLEND) × fog × alpha test × blending. Blending and the colour
  mask are done in the shader (tile buffer colour read), with the same
  coefficients as programs; `SRC_ALPHA_SATURATE` is approximated by `SRC_ALPHA`.
  For `A8` textures the texel's RGB is taken as 1 (GL ES 1.1 environments).
- **Textures:** converted at upload to RGBA8888 (R in byte 0) in Mesa's tiled
  layouts (LT for levels up to 16 pixels, else T), levels smallest first with
  level 0 page aligned, cube faces as whole mip trees; rows bottom-up as sent.
  ETC1 is decoded on the ARM. (Raster RGBA32R, used before, reads rows at a
  stride of max(width, 4) texels: verified with widths 1, 2, 4, 8 and 64.)
- **Output:** the V3D renders RGB565 directly into two alternating screen
  buffers. The ST7789 DMA driver sends them to the panel at 75 MHz (60 fps at
  320×240). On HDMI the V3D renders straight into the pages of a three-page
  firmware framebuffer: one on screen, one waiting for the vertical sync that
  shows it, one being drawn; no copy (a DMA copy cost 2.6 ms of CPU a frame at
  512×300: CPU-G 20% instead of 6%). The vertical sync is the frame count of
  the display scaler's channel for HDMI (HVS `DISPSTAT1`, bits 17:12; measured
  60 a second), polled with a 100 ms timeout. The firmware's "wait for vsync"
  call, used before, has no timeout; with it, the RPi hung once as a monitor
  was switched on during a video (the log stopped, the watchdog reset it;
  where it hung isn't known), and since the change it hasn't. The video demo
  on 1024×600 went from 38–51 to 57–60 fps. A page flip is still a firmware call
  (`SetVirtualOffset`).
- **Screen and HDMI** (`gpu/kernel.cpp`, `gpu/display/`): the kernel command
  line (`cmdline.txt`) sets `output=auto` (the default: HDMI while a monitor is
  connected, else the panel), `output=panel` or `output=hdmi`; `panel=auto`
  (the default) looks for a panel, `panel=yes` and `panel=none` say there is
  one or none (without a panel HDMI keeps the screen without a monitor);
  `hdmi_pixels=N` caps the screen on HDMI. The HDMI screen is the monitor's
  preferred mode (native: 1024×600 for a 1024×600 monitor), up to 1920×1200;
  a larger one, or one over `hdmi_pixels`, is divided by the smallest whole
  number that makes it fit (with `hdmi_pixels=230400`, 1920×1080 and 1280×720
  give 640×360, 1024×600 gives 512×300). The width is a multiple of 16.
- **Video** (`gpu/video/`): MMAL (the Raspberry Pi userland's client,
  BSD-3, vendored in `gpu/video/userland`) talks to the firmware's components
  over Circle's VCHIQ. A stream is `vc.ril.video_decode` tunnelled to
  `vc.ril.isp` inside the VideoCore; the ISP scales to the texture's size and
  converts to RGBA, and its frames come into ARM memory (4 KB aligned buffers,
  by the VideoCore's DMA: the ARM copies nothing). The texture then points at
  the frame: raster RGBA (TMU type RGBA32R). The TMU reads raster rows at a
  power-of-two stride (measured: a 320-wide texture came out garbled, 512
  right), hence the width rule. MMAL's zero-copy would need the firmware's
  VCSM service, which Circle lacks; it isn't needed.
- **VCHIQ's tasks** run when the main loop yields (Circle's cooperative
  scheduler): the loop yields after at most 1 ms of commands. Measured: with
  64 packets a turn and a fast host (a PC over USB), the decoder stopped after
  11 frames.
- **Measured** (720p H.264 test pattern, High profile with B-frames, 512×288
  texture on the panel, from the P4): 30 video frames a second with none
  dropped, GL at 60 fps, the host's CPU 1–2%. Decoder to RGBA in ARM memory:
  96 fps from 720p, 43 fps from 1080p (1024×576).
- **Panel detection** (`CPanelOutput::Detect`): at boot the panel is reset
  and its registers read over MISO (GPIO9), bit-banged at about 500 kHz (the
  ST7789 reads slowly; the driver's SPI runs at 75 MHz), then the pins go back
  to SPI0. A panel answers RDDID (04h: a dummy bit, then its ID bytes) the
  same with MISO pulled up and pulled down, and releases the line after them;
  with nothing there the pull-up reads all ones and the pull-down all zeros.
  Measured: 1000 of 1000 reads the same with each pull (ID 81 81 B3). The ID
  bytes are the module maker's (IDSET, C1h, in the panel's NVM; the ST7789V
  datasheet's default is 85 85 52), and no register tells the glass's size
  (the controller's memory is 240×320 whatever is attached).
- **Hot plug:** the HDMI hot-plug line is GPIO46 on a Zero, GPIO28 on a Zero
  2 W (low while a monitor is connected), sampled every 20 ms; a change counts
  after 200 ms.
  The EDID is read over the DDC bus (BSC2, address 0x50, 100 kHz, about 12 ms),
  because the firmware's EDID property tag keeps answering with the EDID read
  at boot after the monitor is gone. At boot it's read at once; after a hot
  plug from 2 s on (the firmware reads it too, over the same bus, and sets HDMI
  up again); while a connected monitor doesn't answer, it's tried again every
  second.
- **HDMI mode:** the firmware chooses it at boot and doesn't change it later.
  `config.txt` has `hdmi_force_hotplug=1`, so that HDMI stays on (640×480)
  when the RPi boots without a monitor. By default the firmware prefers TV
  modes: for a 1024×600 monitor it sent 720×576 at 50 Hz, capping frames at
  50 fps. `hdmi_group=2` gave 1024×768 at 60 Hz; `hdmi_mode=87` with
  `hdmi_cvt=1024 600 60` gives that monitor's own mode (measured: 59.9 fps).

## 15. Open points

- The exact READY thresholds and ring size, to be tuned against real workloads.
- Whether `DRAW_INLINE` should get 16-bit formats (half the bandwidth for
  screen-space 2D).
- The generic blend ending costs about 75 instructions per fragment; common
  blend states could get cheaper specialised endings.
- A GL ES API wrapper on the Pico (`gl*` names, state queries, `glGetError`).
