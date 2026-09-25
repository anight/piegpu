/*
 * pgpu.h - Pico side of the Pico GPU link (docs/protocol.md)
 *
 * Commands are appended to a staging buffer and sent in batches by DMA over
 * the I2S link. pgpu_flush () sends the batch; pgpu_frame_end () flushes.
 * READY is checked before each batch, idle words keep the clock running.
 *
 * Replies from the Zero are sampled by a second state machine in lockstep
 * with the bit clock, written to a DMA ring, and parsed every millisecond by
 * a timer callback into a small queue (pgpu_poll_reply ()).
 */
#ifndef PGPU_H
#define PGPU_H

#include <stdbool.h>
#include <stdint.h>
#include "pgpu_protocol.h"

#define PGPU_STAGING_WORDS	4096		/* per buffer (two buffers) */
#define PGPU_MAX_REPLY_PAYLOAD	16

typedef struct
{
	uint32_t packets;
	uint32_t words;
	uint32_t batches;
	uint32_t ready_wait_us;		/* time spent waiting for READY */
	uint32_t frames;		/* FRAME pulses seen (total) */
	uint32_t replies;		/* reply packets received */
	uint32_t reply_crc_errors;	/* reply headers with a bad CRC */
	uint32_t reply_overruns;	/* the parser fell behind the DMA ring */
	uint32_t replies_lost;		/* reply queue full */
	uint32_t zero_errors;		/* ERROR replies */
	uint32_t last_error[3];		/* code, opcode, detail */
} pgpu_stats_t;

typedef struct
{
	uint8_t opcode;
	uint8_t length;
	uint32_t payload[PGPU_MAX_REPLY_PAYLOAD];
} pgpu_reply_t;

typedef struct
{
	uint32_t version;
	uint16_t width, height;
	uint32_t max_texture_size;
	uint32_t max_buffers;
	uint32_t max_textures;
	uint32_t max_lights;
	uint32_t ring_bytes;
} pgpu_info_t;

typedef struct
{
	uint32_t frames;
	uint32_t crc_errors;
	uint32_t command_errors;
	uint32_t ring_free_bytes;
	uint32_t last_frame_us;
} pgpu_status_t;

/* link */
void pgpu_init (void);
bool pgpu_wait_ready (uint32_t timeout_ms);	/* the Zero is up and accepting */
bool pgpu_wait_frame (uint32_t timeout_ms);	/* next FRAME pulse */
void pgpu_flush (void);
pgpu_stats_t pgpu_get_stats (void);		/* and reset the counters */

/* replies */
void pgpu_set_reply_phase (unsigned phase);	/* 0: sample at BCLK rise, 1: at BCLK fall */
bool pgpu_poll_reply (pgpu_reply_t *reply);
/* wait for a reply with this opcode; other replies are discarded (ERROR is counted) */
bool pgpu_wait_reply (uint8_t opcode, pgpu_reply_t *reply, uint32_t timeout_ms);
bool pgpu_get_info (pgpu_info_t *info, uint32_t timeout_ms);
bool pgpu_get_status (pgpu_status_t *status, uint32_t timeout_ms);
/* round trip: -1 on timeout, else microseconds */
int32_t pgpu_ping_wait (uint32_t cookie, uint32_t timeout_ms);

/* raw packet building: returns the payload to fill, then pgpu_end () */
uint32_t *pgpu_begin (uint8_t opcode, uint32_t payload_words);
void pgpu_end (void);

/* system and frame */
void pgpu_reset (void);
void pgpu_ping (uint32_t cookie);
void pgpu_request_info (void);
void pgpu_request_status (void);
void pgpu_clear (uint32_t mask, uint32_t color, float depth);
void pgpu_frame_end (uint32_t flags);
void pgpu_viewport (int32_t x, int32_t y, uint32_t width, uint32_t height, float near, float far);

/* buffers (data is split into packets as needed) */
void pgpu_buffer_create (uint32_t id, uint32_t size_bytes);
void pgpu_buffer_data (uint32_t id, uint32_t offset_bytes, const void *data, uint32_t length_bytes);
void pgpu_buffer_delete (uint32_t id);

/* textures (pixel rows bottom-up, each row padded to 4 bytes) */
void pgpu_texture_create (uint32_t id, uint32_t width, uint32_t height, uint32_t format);
void pgpu_texture_data (uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
			uint32_t format, const void *pixels);
void pgpu_texture_params (uint32_t id, uint32_t min_filter, uint32_t mag_filter,
			  uint32_t wrap_s, uint32_t wrap_t);
void pgpu_texture_delete (uint32_t id);
void pgpu_texture_bind (uint32_t id);
void pgpu_tex_env (uint32_t mode, uint32_t env_color);

/* fragment state */
void pgpu_enable (uint32_t caps);
void pgpu_disable (uint32_t caps);
void pgpu_depth_func (uint32_t func);
void pgpu_depth_mask (bool write);
void pgpu_blend_func (uint32_t src, uint32_t dst);
void pgpu_cull_face (uint32_t face);
void pgpu_front_face (uint32_t winding);
void pgpu_alpha_func (uint32_t func, float ref);
void pgpu_color_mask (bool r, bool g, bool b, bool a);
void pgpu_scissor (int32_t x, int32_t y, uint32_t width, uint32_t height);	/* ENABLE SCISSOR_TEST */
void pgpu_polygon_offset (float factor, float units);	/* ENABLE POLYGON_OFFSET_FILL */
void pgpu_line_width (float width);

/* transform, lighting, fog, current values */
void pgpu_load_matrix (uint32_t which, const float m[16]);
void pgpu_light (uint32_t light, const float position[4], uint32_t ambient, uint32_t diffuse,
		 uint32_t specular, const float attenuation[3]);
void pgpu_material (uint32_t ambient, uint32_t diffuse, uint32_t specular, uint32_t emission,
		    float shininess);
void pgpu_light_model (uint32_t ambient, bool two_side);
void pgpu_fog (uint32_t mode, uint32_t color, float start, float end, float density);
void pgpu_shade_model (uint32_t mode);
void pgpu_color (uint32_t color);
void pgpu_normal (float x, float y, float z);
void pgpu_texcoord (float s, float t);

/* arrays and drawing */
void pgpu_array (uint32_t attribute, uint32_t buffer, uint32_t offset_bytes, uint32_t stride_bytes,
		 uint32_t size, uint32_t type);
void pgpu_arrays_enable (uint32_t mask);
void pgpu_draw_arrays (uint32_t mode, uint32_t first, uint32_t count);
void pgpu_draw_elements (uint32_t mode, uint32_t count, uint32_t index_type, uint32_t buffer,
			 uint32_t offset_bytes);

/* programs (GL ES 2.0 subset, docs/protocol.md 7.10): blobs and uniform
   tables come from the headers tools/glslc/glslc.py generates */
void pgpu_program_create (uint32_t id, const uint32_t *blob, uint32_t words);
void pgpu_program_delete (uint32_t id);
void pgpu_use_program (uint32_t id);		/* 0 = fixed function */
/* set a uniform: offsets = the uniform's table from the generated header
   (scalars vertex-stage offsets, then scalars fragment-stage offsets) */
void pgpu_program_uniform (uint32_t id, const uint16_t *offsets, uint32_t scalars, const float *values);
#define PGPU_UNIFORM(id, table, values) \
	pgpu_program_uniform (id, table, sizeof (table) / sizeof (table[0]) / 2, values)
void pgpu_program_uniform1f (uint32_t id, const uint16_t *offsets, float value);	/* single float */
void pgpu_program_sampler (uint32_t id, uint32_t sampler, uint32_t unit);
void pgpu_texture_bind_unit (uint32_t unit, uint32_t texture);
void pgpu_vertex_attrib (uint32_t index, float x, float y, float z, float w);
void pgpu_attrib_array (uint32_t index, uint32_t buffer, uint32_t offset_bytes, uint32_t stride_bytes,
			uint32_t size, uint32_t type);
void pgpu_attribs_enable (uint32_t mask);

/* client-side arrays for programs (like glVertexAttribPointer without a
   buffer): the data stays in Pico memory and the draw calls below copy the
   vertices used into PROGRAM_DRAW_INLINE packets. type and size must be the
   program's attribute format; stride 0 = tightly packed; pointer NULL =
   the attribute doesn't come from Pico memory. Large TRIANGLES, LINES and
   POINTS draws are split into several packets; other modes and indexed
   draws must fit one packet (false otherwise). */
void pgpu_client_attrib_pointer (uint32_t index, uint32_t size, uint32_t type, uint32_t stride,
				 const void *pointer);
bool pgpu_draw_arrays_client (uint32_t mode, uint32_t first, uint32_t count);
bool pgpu_draw_elements_client (uint32_t mode, uint32_t count, uint32_t index_type, const void *indices);

/* DRAW_INLINE: vertices laid out as in docs/protocol.md 7.8 */
void pgpu_draw_inline (uint32_t mode, uint32_t count, uint32_t attrib_mask,
		       const uint32_t *vertex_words, uint32_t words_per_vertex);

#endif
