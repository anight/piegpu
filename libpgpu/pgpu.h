/*
 * pgpu.h - Pico side of the Pico GPU link (docs/protocol.md)
 *
 * Commands are appended to a staging buffer and sent in batches by the
 * transport (pgpu_link.h: the I2S link on the Pico, pgpu_pico.c; the Zero's
 * USB on a PC, host/pgpu_host.c). pgpu_flush () sends the batch;
 * pgpu_frame_end () flushes. Replies are parsed into small queues
 * (pgpu_poll_reply (), pgpu_poll_error ()).
 */
#ifndef PGPU_H
#define PGPU_H

#include <stdbool.h>
#include <stdint.h>
#include "pgpu_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PGPU_STAGING_WORDS	4096		/* per buffer (two buffers) */
#define PGPU_MAX_REPLY_PAYLOAD	64

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
	uint32_t errors_lost;		/* error queue full */
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
	/* the Zero's last measuring window (about a second; 0 from an older Zero) */
	uint32_t window_us;
	uint32_t window_frames;
	uint32_t v3d_busy_us;		/* binning and rendering */
	uint32_t arm_busy_us;		/* receiving and executing, not waiting */
	uint32_t panel_wait_us;		/* for the panel DMA of the previous frame */
} pgpu_status_t;

/* the screen and the HDMI monitor (the DISPLAY reply, docs/protocol.md 9) */
typedef struct
{
	uint8_t output;			/* PGPU_OUTPUT_PANEL or PGPU_OUTPUT_HDMI: where frames go */
	bool hdmi_connected;		/* a monitor is plugged in */
	bool panel_present;		/* the Zero has a panel (configured) */
	bool edid;			/* the monitor's EDID was read: the monitor fields are set */
	uint16_t width, height;		/* the screen (framebuffer 0) */
	uint16_t monitor_width;		/* the monitor's preferred mode */
	uint16_t monitor_height;
	uint32_t monitor_refresh_mhz;	/* its refresh rate in millihertz */
	uint16_t signal_width;		/* the HDMI mode the Zero sends (the firmware's) */
	uint16_t signal_height;
	char monitor_name[14];		/* from the EDID, may be empty */
} pgpu_display_t;

/* the last DISPLAY reply; returns how many have arrived (0: none yet, the
   Zero sends one after each INFO and whenever something changes). Replies
   never get lost to other waits: compare the count to see a change */
uint32_t pgpu_get_display (pgpu_display_t *display);

/* video streams decoded by the Zero into textures (docs/protocol.md 7.12) */
typedef struct
{
	uint32_t flags;			/* PGPU_VIDEO_OPEN_FLAG, _PLAYING, _ENDED, _ERROR */
	uint32_t bytes_done;		/* sample bytes the decoder has taken, since the open */
	uint32_t ring_bytes;		/* the Zero's buffer for the samples not taken yet */
	uint32_t decoded, shown, dropped;	/* frames */
	int64_t shown_pts;		/* the frame in the texture (PGPU_VIDEO_TIME_NONE: none) */
	uint32_t waiting;		/* decoded frames waiting for their time */
	uint32_t samples_done;		/* samples the decoder has taken, since the open */
	uint32_t max_samples;		/* samples the Zero holds (not taken yet) */
} pgpu_video_status_t;

/* where data comes from: bytes at offset into buffer (a file on an SD card
   through its filesystem, memory, ...); false if it can't */
typedef bool (*pgpu_read_t) (void *ctx, uint64_t offset, void *buffer, uint32_t bytes);

/* texture (Zero id) becomes an RGBA width x height video texture (width a
   power of two, 32 or more; height a multiple of 16), fed by stream (1 or 2):
   H.264 of coded_width x coded_height, scaled to the texture. avcc: the
   samples are NAL units with length prefixes, as in MP4 (pgpu_mp4: its avcC);
   NULL: Annex B (start codes, SPS and PPS in the stream) */
void pgpu_video_open (uint32_t stream, uint32_t texture, uint32_t width, uint32_t height,
		      uint32_t coded_width, uint32_t coded_height, const void *avcc, uint32_t avcc_bytes);
/* a whole sample (an access unit; flags PGPU_VIDEO_KEYFRAME, _CONFIG, _EOS),
   pts in microseconds; split into packets as needed. Send only what
   pgpu_video_room () allows: the Zero rejects the rest */
void pgpu_video_sample (uint32_t stream, uint32_t flags, int64_t pts, const void *data, uint32_t bytes);
/* the same, the data read straight into the packets (bytes at offset through
   read, a packet's payload at a time: no buffer for the sample); false if a
   read failed (the Zero drops the part it has) */
bool pgpu_video_sample_read (uint32_t stream, uint32_t flags, int64_t pts, uint32_t bytes,
			     pgpu_read_t read, void *ctx, uint64_t offset);
uint32_t pgpu_video_room (uint32_t stream);
void pgpu_video_control (uint32_t stream, uint32_t op, int64_t arg);	/* PGPU_VIDEO_PLAY, ... */
void pgpu_video_request_status (uint32_t stream);
/* the last VIDEO_STATUS of a stream (the Zero sends one every 100 ms while it's
   open); returns how many have come (0: none) */
uint32_t pgpu_video_get_status (uint32_t stream, pgpu_video_status_t *status);

/* link */
void pgpu_init (void);
/* the side-band signals (the transport's; over USB they answer at once) */
bool pgpu_wait_ready (uint32_t timeout_ms);	/* the Zero is up and accepting */
bool pgpu_wait_frame (uint32_t timeout_ms);	/* next FRAME pulse */
uint32_t pgpu_frame_count (void);		/* FRAME pulses so far (0 over USB) */
uint64_t pgpu_time_us (void);			/* the transport's clock, microseconds */
void pgpu_flush (void);
uint32_t pgpu_packets_sent (void);		/* packets built since pgpu_init () */
pgpu_stats_t pgpu_get_stats (void);		/* and reset the counters */

/* replies */
void pgpu_set_reply_phase (unsigned phase);	/* 0: sample at BCLK rise, 1: at BCLK fall (if the transport can) */
bool pgpu_poll_reply (pgpu_reply_t *reply);		/* other than ERROR and PIXELS */
bool pgpu_poll_error (uint32_t error[3]);		/* ERROR reply: code, opcode, detail */
/* wait for a reply with this opcode; other replies are discarded */
bool pgpu_wait_reply (uint8_t opcode, pgpu_reply_t *reply, uint32_t timeout_ms);
bool pgpu_get_info (pgpu_info_t *info, uint32_t timeout_ms);
bool pgpu_get_status (pgpu_status_t *status, uint32_t timeout_ms);
/* a STATUS reply (from pgpu_poll_reply after pgpu_request_status) */
bool pgpu_status_from_reply (const pgpu_reply_t *reply, pgpu_status_t *status);
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

/* framebuffers: render to level 0 of a texture (face 0-5 for cube maps),
   optionally with depth and stencil (PGPU_FRAMEBUFFER_DEPTH_STENCIL) */
void pgpu_framebuffer_create (uint32_t id, uint32_t texture, uint32_t face, uint32_t flags);
void pgpu_framebuffer_delete (uint32_t id);
void pgpu_bind_framebuffer (uint32_t id);	/* 0 = the panel */
/* read RGBA8888 pixels of the bound framebuffer (GL coordinates, rows bottom up) */
bool pgpu_read_pixels (int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t *pixels,
		       uint32_t timeout_ms);
/* like glCopyTexSubImage2D: from the bound framebuffer into a texture level */
void pgpu_copy_tex_image (uint32_t texture, uint32_t level, uint32_t face, uint32_t xoffset,
			  uint32_t yoffset, int32_t x, int32_t y, uint32_t width, uint32_t height);

/* buffers (data is split into packets as needed) */
void pgpu_buffer_create (uint32_t id, uint32_t size_bytes);
void pgpu_buffer_data (uint32_t id, uint32_t offset_bytes, const void *data, uint32_t length_bytes);
void pgpu_buffer_delete (uint32_t id);

/* textures (pixel rows bottom-up, each row padded to 4 bytes) */
void pgpu_texture_create (uint32_t id, uint32_t width, uint32_t height, uint32_t format);
void pgpu_texture_create_cube (uint32_t id, uint32_t size, uint32_t format);
/* upload to a mip level (0 = base) and cube face (0-5: +X, -X, +Y, -Y, +Z, -Z) */
void pgpu_texture_data_level (uint32_t id, uint32_t level, uint32_t face, uint32_t x, uint32_t y,
			      uint32_t width, uint32_t height, uint32_t format, const void *pixels);
/* the same with rows src_stride bytes apart (GL_UNPACK_ALIGNMENT) */
void pgpu_texture_data_stride (uint32_t id, uint32_t level, uint32_t face, uint32_t x, uint32_t y,
			       uint32_t width, uint32_t height, uint32_t format, const void *pixels,
			       uint32_t src_stride);
void pgpu_generate_mipmap (uint32_t id);
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
void pgpu_blend_func_separate (uint32_t src_rgb, uint32_t dst_rgb, uint32_t src_alpha, uint32_t dst_alpha);
void pgpu_blend_equation (uint32_t mode_rgb, uint32_t mode_alpha);
void pgpu_blend_color (float r, float g, float b, float a);
void pgpu_stencil_func (uint32_t face, uint32_t func, uint32_t ref, uint32_t mask);	/* ENABLE STENCIL_TEST */
void pgpu_stencil_op (uint32_t face, uint32_t fail, uint32_t zfail, uint32_t zpass);
void pgpu_stencil_mask (uint32_t face, uint32_t mask);
void pgpu_clear_stencil (uint32_t mask, uint32_t color, float depth, uint32_t stencil);	/* CLEAR with stencil */
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
/* the most vertices pgpu_draw_arrays_client sends in one packet (more are
   split there, which buffer arrays mixed in can't follow: callers split) */
uint32_t pgpu_client_max_vertices (void);
bool pgpu_draw_arrays_client (uint32_t mode, uint32_t first, uint32_t count);
bool pgpu_draw_elements_client (uint32_t mode, uint32_t count, uint32_t index_type, const void *indices);

/* DRAW_INLINE: vertices laid out as in docs/protocol.md 7.8 */
void pgpu_draw_inline (uint32_t mode, uint32_t count, uint32_t attrib_mask,
		       const uint32_t *vertex_words, uint32_t words_per_vertex);

#ifdef __cplusplus
}
#endif

#endif
