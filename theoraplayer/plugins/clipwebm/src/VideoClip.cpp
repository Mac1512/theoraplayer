/// @file
/// @version 1.0
/// 
/// @section LICENSE
/// 
/// This program is free software; you can redistribute it and/or modify it under
/// the terms of the BSD license: http://opensource.org/licenses/BSD-3-Clause

#include <string>

#include <theoraplayer/Exception.h>
#include <theoraplayer/FrameQueue.h>
#include <theoraplayer/Manager.h>
#include <theoraplayer/PixelTransform.h>
#include <theoraplayer/Timer.h>
#include <theoraplayer/VideoClip.h>
#include <theoraplayer/VideoFrame.h>
#include <tools_common.h>

#include "Utility.h"
#include "VideoClip.h"
#include "webmdec.h"

namespace clipwebm
{
	VideoClip::VideoClip(theoraplayer::DataSource* dataSource, theoraplayer::TheoraOutputMode outputMode, int precachedFramesCount, bool usePotStride) :
		theoraplayer::VideoClip(dataSource, outputMode, precachedFramesCount, usePotStride),
		AudioPacketQueue()
	{
		memset(&(webmContext), 0, sizeof(webmContext));
		this->input.webmContext = &webmContext;
		this->input.vpxInputContext = &vpxInputContext;
		this->seekFrame = 0;
		this->frameNumber = 0;
	}

	theoraplayer::VideoClip* VideoClip::create(theoraplayer::DataSource* dataSource, theoraplayer::TheoraOutputMode outputMode, int precachedFramesCount, bool usePotStride)
	{
		return new VideoClip(dataSource, outputMode, precachedFramesCount, usePotStride);
	}

	VideoClip::~VideoClip()
	{
		webm_free(this->input.webmContext);
	}

	bool VideoClip::_readData()
	{
		return true;
	}

	bool VideoClip::decodeNextFrame()
	{
		theoraplayer::VideoFrame* frame = this->frameQueue->requestEmptyFrame();
		if (frame == NULL)
		{
			return false; // max number of precached frames reached
		}
		bool shouldRestart = false;
		uint8_t* buf = NULL;
		size_t bytesInBuffer = 0;
		size_t bufferSize = 0;
		if (!webm_read_frame(this->input.webmContext, &buf, &bytesInBuffer, &bufferSize))
		{
			vpx_codec_iter_t iter = NULL;
			vpx_image_t* img = NULL;
			if (vpx_codec_decode(&decoder, buf, (unsigned int)bytesInBuffer, NULL, 0))
			{
				const char* detail = vpx_codec_error_detail(&decoder);
				if (detail != NULL)
				{
					theoraplayer::log("Additional information: " + std::string(detail));
				}
			}
			if ((img = vpx_codec_get_frame(&decoder, &iter)))
			{
				this->frame = img;
				frame->timeToDisplay = this->frameNumber / this->fps;
				frame->iteration = this->iteration;
				frame->_setFrameNumber(this->frameNumber);
				++this->frameNumber;
				this->lastDecodedFrameNumber = this->frameNumber;
				if (this->lastDecodedFrameNumber >= (unsigned long)this->numFrames)
				{
					shouldRestart = true;
				}
				PixelTransform t;
				memset(&t, 0, sizeof(PixelTransform));
				t.y = this->frame->planes[0]; t.yStride = this->frame->stride[0];
				t.u = this->frame->planes[1]; t.uStride = this->frame->stride[1];
				t.v = this->frame->planes[2]; t.vStride = this->frame->stride[2];
				frame->decode(&t);
			}
		}
		return true;
	}

	void VideoClip::_restart()
	{
		bool paused = this->timer->isPaused();
		if (!paused)
		{
			this->timer->pause();
		}
		webm_rewind(input.webmContext);
		this->frameNumber = 0;
		this->lastDecodedFrameNumber = -1;
		this->seekFrame = 0;
		this->endOfFile = false;
		this->restarted = true;
		if (!paused)
		{
			this->timer->play();
		}
	}

	void VideoClip::_load(theoraplayer::DataSource* source)
	{
		if (!file_is_webm(source, input.webmContext, input.vpxInputContext))
		{
			theoraplayer::log("ERROR: File is not webm.");
			return;
		}
		if (webm_guess_framerate(source, input.webmContext, input.vpxInputContext))
		{
			theoraplayer::log("ERROR: Unable to guess webm framerate.");
			return;
		}
		this->numFrames = webm_guess_duration(input.webmContext);
		webm_rewind(input.webmContext);
#ifdef _DEBUG
		float fps = (float)input.vpxInputContext->framerate.numerator / (float)input.vpxInputContext->framerate.denominator;
		theoraplayer::log("Framerate: " + strf(fps));
#endif
		this->width = input.vpxInputContext->width;
		this->height = input.vpxInputContext->height;
		this->subFrameWidth = input.vpxInputContext->width;
		this->subFrameHeight = input.vpxInputContext->height;
		this->subFrameOffsetX = 0;
		this->subFrameOffsetY = 0;
		this->stride = (this->stride == 1) ? potCeil(getWidth()) : getWidth();
		this->fps = (float)input.vpxInputContext->framerate.numerator / (float)input.vpxInputContext->framerate.denominator;
		this->frameDuration = 1.0f / this->fps;
		this->duration = this->numFrames * this->frameDuration;
#ifdef _DEBUG
		theoraplayer::log("Video duration: " + strf(this->duration));
#endif
		this->fourccInterface = (VpxInterface*)get_vpx_decoder_by_fourcc(vpxInputContext.fourcc);
		int decoderFlags = 0;
		if (vpx_codec_dec_init(&decoder, this->fourccInterface->codec_interface(), &cfg, decoderFlags))
		{
			theoraplayer::log("Error: Failed to initialize decoder: " + std::string(vpx_codec_error(&decoder)));
		}
		else if (this->frameQueue == NULL)
		{
			this->frameQueue = new theoraplayer::FrameQueue(this);
			this->frameQueue->setSize(this->precachedFramesCount);
		}
	}
	
	void VideoClip::decodedAudioCheck()
	{
		if (this->audioInterface != NULL && !this->timer->isPaused())
		{
			this->_flushSynchronizedAudioPackets(this->audioInterface, this->audioMutex);
		}
	}

	float VideoClip::decodeAudio()
	{
		return -1;
	}

	void VideoClip::doSeek()
	{
		float time = this->seekFrame / getFps();
		this->timer->seek(time);
		bool paused = this->timer->isPaused();
		if (!paused)
		{
			this->timer->pause();
		}
		this->resetFrameQueue();
#ifdef _DEBUG
		theoraplayer::log("Seek frame: " + str(this->seekFrame));
#endif
		this->lastDecodedFrameNumber = this->seekFrame;
		if (!paused)
		{
			this->timer->play();
		}
		this->seekFrame = -1;
	}

}
