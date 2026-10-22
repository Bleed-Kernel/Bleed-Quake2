#include "../client/client.h"
#include "../client/snd_loc.h"

#include <bleed_audio.h>

#define STREAM_FRAMES	16384	// 340ms at 48k

static au_stream_t stream;

qboolean SNDDMA_Init(void)
{
	if (au_open() < 0)
	{
		Com_Printf("SNDDMA_Init: no sound card\n");
		return false;
	}
	if (au_stream_open(STREAM_FRAMES, &stream) < 0)
	{
		Com_Printf("SNDDMA_Init: couldnt open a stream\n");
		au_close();
		return false;
	}

	dma.samplebits = 16;
	dma.channels = 2;
	dma.speed = au_rate();
	dma.samples = stream.frames * dma.channels;
	dma.submission_chunk = 1;
	dma.samplepos = 0;
	dma.buffer = (byte *)stream.ring;

	Com_Printf("SNDDMA_Init: %d Hz stereo through libaudio\n", dma.speed);
	return true;
}

int SNDDMA_GetDMAPos(void)
{
	return (int)((au_stream_pos() % stream.frames) * dma.channels);
}

void SNDDMA_Shutdown(void)
{
	au_stream_close();
	au_close();
	dma.buffer = NULL;
}

void SNDDMA_BeginPainting(void)
{
}

void SNDDMA_Submit(void)
{
}
