/*
  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include "str.h"
#include "psx.h"
#include "gfx.h"
#include "psn00b/strnoob.h"
#include "mem.h"
#include "psx/timer.h"
#include "psx/stage.h"

/* CD and MDEC interrupt handlers */
#define BLOCK_SIZE 16

#define VRAM_X_COORD(x) ((x) * BLOCK_SIZE / 16)

// All non-audio sectors in .STR files begin with this 32-byte header, which
// contains metadata about the sector and is followed by a chunk of frame
// bitstream data.
// https://problemkaputt.de/psx-spx.htm#cdromfilevideostrstreamingandbspicturecompressionsony
typedef struct 
{
	u16 magic;			// Always 0x0160
	u16 type;			// 0x8001 for MDEC
	u16 sector_id;		// Chunk number (0 = first chunk of this frame)
	u16 sector_count;	// Total number of chunks for this frame
	u32 frame_id;		// Frame number
	u32 bs_length;		// Total length of this frame in bytes

	u16 width, height;
	u8  bs_header[8];
	u32 _reserved;
} STR_Header;

typedef struct 
{
	u16 width, height;
	u32 bs_data[0x2000];	// Bitstream data read from the disc
	u32 mdec_data[0x8000];	// Decompressed data to be fed to the MDEC
} StreamBuffer;

typedef struct 
{
	StreamBuffer frames[2];
	u32     slices[2][BLOCK_SIZE * SCREEN_HEIGHT2];

	int  frame_id, sector_count;
	int  dropped_frames;
	RECT slice_pos;
	int  frame_width;

	volatile s8 sector_pending, frame_ready;
	volatile s8 cur_frame, cur_slice;
} StreamContext;

typedef struct
{
	const char* name;
	StageId id;
} STR_Def;

static StreamContext* str_ctx;

// This buffer is used by cd_sector_handler() as a temporary area for sectors
// read from the CD. Due to DMA limitations it can't be allocated on the stack
// (especially not in the interrupt callbacks' stack, whose size is very
// limited).
static STR_Header* sector_header;

static const STR_Def str_def[] = {
	#include "strdef.h"
};

void cd_sector_handler(void) 
{
	StreamBuffer *frame = &str_ctx->frames[str_ctx->cur_frame];

	// Fetch the .STR header of the sector that has been read and make sure it
	// is valid. If not, assume the file has ended and set frame_ready as a
	// signal for the main loop to stop playback.
	CdGetSector(sector_header, sizeof(STR_Header) / 4);

	if (sector_header->magic != 0x0160) 
	{
		str_ctx->frame_ready = -1;
		return;
	}

	// Ignore any non-MDEC sectors that might be present in the stream.
	if (sector_header->type != 0x8001)
		return;

	// If this sector is actually part of a new frame, validate the sectors
	// that have been read so far and flip the bitstream data buffers. If the
	// frame number is actually lower than the current one, assume the drive
	// has started reading another .STR file and stop playback.
	if ((int) sector_header->frame_id < str_ctx->frame_id) 
	{
		str_ctx->frame_ready = -1;
		return;
	}

	if ((int) sector_header->frame_id > str_ctx->frame_id) 
	{
		// Do not set the ready flag if any sector has been missed.
		if (str_ctx->sector_count)
			str_ctx->dropped_frames++;
		else
			str_ctx->frame_ready = 1;

		str_ctx->frame_id     = sector_header->frame_id;
		str_ctx->sector_count = sector_header->sector_count;
		str_ctx->cur_frame   ^= 1;

		frame = &str_ctx->frames[str_ctx->cur_frame];

		// Initialize the next frame. Dimensions must be rounded up to the
		// nearest multiple of 16 as the MDEC operates on 16x16 pixel blocks.
		frame->width  = (sector_header->width  + 15) & 0xfff0;
		frame->height = (sector_header->height + 15) & 0xfff0;
	}

	// Append the payload contained in this sector to the current buffer.
	str_ctx->sector_count--;
	CdGetSector(
		&(frame->bs_data[2016 / 4 * sector_header->sector_id]),
		2016 / 4
	);
}

void mdec_dma_handler(void) 
{
	// Handle any sectors that were not processed by cd_event_handler() (see
	// below) while a DMA transfer from the MDEC was in progress. As the MDEC
	// has just finished decoding a slice, they can be safely handled now.
	if (str_ctx->sector_pending) 
	{
		cd_sector_handler();
		str_ctx->sector_pending = 0;
	}

	// Upload the decoded slice to VRAM and start decoding the next slice (into
	// another buffer) if any.
	LoadImage(&str_ctx->slice_pos, str_ctx->slices[str_ctx->cur_slice]);

	str_ctx->cur_slice   ^= 1;
	str_ctx->slice_pos.x += BLOCK_SIZE;

	if (str_ctx->slice_pos.x < str_ctx->frame_width)
		DecDCTout(
			str_ctx->slices[str_ctx->cur_slice],
			BLOCK_SIZE * str_ctx->slice_pos.h / 2
		);
}

void cd_event_handler(u8 event, u8 *payload) 
{
	// Ignore all events other than a sector being ready.
	if (event != CdlDataReady)
		return;

	// Only handle sectors immediately if the MDEC is not decoding a frame,
	// otherwise defer handling to mdec_dma_handler(). This is a workaround for
	// a hardware conflict between the DMA channels used for the CD drive and
	// MDEC output, which shall not run simultaneously.
	if (DecDCTinSync(1))
		str_ctx->sector_pending = 1;
	else
		cd_sector_handler();
}

StreamBuffer *get_next_frame(void) 
{
	while (!str_ctx->frame_ready)
		__asm__ volatile("");

	if (str_ctx->frame_ready < 0)
	{
		return 0;
	}

	str_ctx->frame_ready = 0;
	return &str_ctx->frames[str_ctx->cur_frame ^ 1];
}

static void STR_InitStream(void)
{
	EnterCriticalSection();
	DecDCToutCallback(&mdec_dma_handler);
	CdReadyCallback(&cd_event_handler);
	ExitCriticalSection();

	// Copy the lookup table used for frame decompression to the scratchpad
	// area. This is optional but makes the decompressor slightly faster. See
	// the libpsxpress documentation for more details.
	DecDCTvlcCopyTableV3((VLC_TableV3*) 0x1f800000);

	stage.movie_is_playing = true;
	stage.movie_pos = 0;
	stage.audio_last_pos_before_movie = Audio_TellXA_Milli();

	str_ctx->cur_frame = 0;
  str_ctx->cur_slice = 0;
}

static void STR_StopStream(void)
{
	CdControlB(CdlPause, 0, 0);
  EnterCriticalSection();
  CdReadyCallback(NULL);
  DecDCToutCallback(NULL);
  ExitCriticalSection();
  stage.movie_is_playing = false;
}

static void Str_Update(void)
{
	StreamBuffer *frame = get_next_frame();

	// Wait for a full frame to be read from the disc and decompress the
	// bitstream into the format expected by the MDEC. If the video has
	// ended, restart playback from the beginning.
	if (!frame || pad_state.press & PAD_START) 
	{
		STR_StopStream();
		Mem_Free(str_ctx);
		Mem_Free(sector_header);
		return;
	}

	VLC_Context vlc_ctx;
	DecDCTvlcStart(&vlc_ctx, frame->mdec_data, sizeof(frame->mdec_data) / 4, frame->bs_data);

	Gfx_FlipWithoutOT();

	if (stage.note_scroll >= 0)
		Stage_Tick();

	DrawSync(0);

  // Feed the newly decompressed frame to the MDEC. The MDEC will not
	// actually start decoding it until an output buffer is also configured
	// by calling DecDCTout() (see below).
	DecDCTin(frame->mdec_data, DECDCT_MODE_16BPP);

	// Place the frame at the center of the currently active framebuffer
	// and start decoding the first slice. Decoded slices will be uploaded
	// to VRAM in the background by mdec_dma_handler().
	RECT *fb_clip = Gfx_GetDrawClip();
	int  x_offset = (fb_clip->w - frame->width)  / 2;
	int  y_offset = (fb_clip->h - frame->height) / 2;

	str_ctx->slice_pos.x = fb_clip->x + VRAM_X_COORD(x_offset);
	str_ctx->slice_pos.y = fb_clip->y + y_offset;
	str_ctx->slice_pos.w = BLOCK_SIZE;
	str_ctx->slice_pos.h = frame->height;
	str_ctx->frame_width = VRAM_X_COORD(frame->width);

	DecDCTout(
		str_ctx->slices[str_ctx->cur_slice],
		BLOCK_SIZE * str_ctx->slice_pos.h / 2
	);
}

void Str_Init(void)
{
	DecDCTReset(0);
	stage.movie_is_playing = false;
}

void Str_PlayFile(CdlFILE* file)
{
	str_ctx = Mem_Alloc(sizeof(StreamContext));
	sector_header = Mem_Alloc(sizeof(STR_Header));
	STR_InitStream();

	str_ctx->frame_id       = -1;
	str_ctx->dropped_frames =  0;
	str_ctx->sector_pending =  0;
	str_ctx->frame_ready    =  0;

	// Configure the CD drive to read at 2x speed and to play any XA-ADPCM
	// sectors that might be interleaved with the video data.
	u8 mode = CdlModeRT | CdlModeSpeed;
	CdControlB(CdlSetmode, (u8*) &mode, 0);

	// Start reading in real-time mode (i.e. without retrying in case of read
	// errors) and wait for the first frame to be buffered.
	CdControl(CdlReadS, (u8*)&file->pos, 0);

	get_next_frame();

	// Random ass number so the chart sync with the music (this probably will not work on hardware)
	stage.movie_pos -= (5 * 75);
	while (stage.movie_is_playing)
	{
		Timer_Tick();
		Pad_Update();
		Str_Update();
		stage.movie_pos += timer_dt;
	}

	Gfx_EnableClear();

	//Prepare CD drive for XA reading
	mode = CdlModeRT | CdlModeSF | CdlModeSize1;
	
	CdControlB(CdlSetmode, &mode, NULL);
	CdControlF(CdlPause, NULL);

}

void Str_Play(const char *filedir)
{
	CdlFILE file;

	IO_FindFile(&file, filedir);
	CdSync(0, 0);

	Str_PlayFile(&file);
}

void Str_CanPlayDef(void)
{
	//Check if have some movie to play
	 for (u8 i = 0; i < COUNT_OF(str_def); i++)
	 {
	   //Play only in story mode
	   if (str_def[i].id == stage.stage_id && stage.story)
			Str_Play(str_def[i].name);
	}
}