#include "Decoder.h"
#include "VideoSource.h"

#include <memory>
#include <mutex>

extern "C" { 
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace svi {

	struct DecoderDetail
	{
		std::shared_ptr<AVFormatContext> format;
		std::shared_ptr<AVCodecContext> codecCtx;
		std::shared_ptr<AVFrame> currentFrame;

		struct SwsContext *swsCtx;
		int videoStreamIdx;
		bool isOpened;
		int BGR24Linesize[AV_NUM_DATA_POINTERS];

		int videoWidth;
		int videoHeight;

		DecoderDetail()
			: swsCtx(nullptr),
			  videoStreamIdx(-1),
			  isOpened(false),
			  videoWidth(0),
			  videoHeight(0)
		{}
	};

}


// Use RAII to ensure av_free_packet is called.
// Based on http://blog.tomaka17.com/2012/03/libavcodeclibavformat-tutorial

struct SafeAVPacket
{
	AVPacket packet;

	SafeAVPacket()
	{
		packet.data = nullptr;
	}

	virtual ~SafeAVPacket()
	{
		freeData();
	}

	int reset(AVFormatContext* formatCtx)
	{
		freeData();

		int result = av_read_frame(formatCtx, &packet);
		if (result < 0)
			packet.data = nullptr;
		return result;
	}

	void freeData()
	{
		if (packet.data)
			av_free_packet(&packet);
	}

};

/******************************************************************************/
/*                                   Decoder                                  */

using namespace svi;

Decoder::Decoder() :
	m_detail(new DecoderDetail)
{
	;;
}

Decoder::Decoder(const std::string & fileName) :
	m_detail(new DecoderDetail)
{
	openFormatContext(fileName);
}

Decoder::Decoder(const VideoSource & videoSource)	:
	m_detail(new DecoderDetail)
{
	openFormatContext(std::string(), videoSource.getAVIO());
}

Decoder::~Decoder()
{
	delete m_detail;
}

bool Decoder::open(const std::string & fileName)
{
	try {
		if (isOpened())
			release();
		openFormatContext(fileName);
		return true;
	} catch (...) {
		return false;
	}
}

void Decoder::openFormatContext(const std::string & fileName, AVIOContext *ioCtx)
{
	initLibavcodec();
	
	///////////////////
	// Open file

	AVFormatContext *pFormatCtx = nullptr;
	if (ioCtx) {
		pFormatCtx = avformat_alloc_context();
		pFormatCtx->pb = ioCtx;
	}

	if (avformat_open_input(&pFormatCtx, fileName.c_str(), nullptr, nullptr) != 0)
		throw std::runtime_error("Cannot open file: Does not exist or is no supported format");

	m_detail->format
		= std::shared_ptr<AVFormatContext>(pFormatCtx,
										   [](AVFormatContext *format)
										   {
											   avformat_close_input(&format);
										   });
	if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
		throw std::runtime_error("File contains no streams");

	findFirstVideoStream();
	openCodec();
	prepareTargetBuffer();
	prepareResizeContext();

	m_detail->isOpened = true;
}

bool Decoder::isOpened() const
{
	return m_detail->isOpened;
}

void Decoder::release()
{
	std::unique_ptr<DecoderDetail> oldDetail(m_detail);

	m_detail = new DecoderDetail;
}

long Decoder::millisecondsPerFrame() const
{
	double frame_delay = av_q2d(m_detail->codecCtx->time_base);
	return m_detail->codecCtx->ticks_per_frame * 1000 * frame_delay;
}

int Decoder::videoWidth() const
{
	return m_detail->videoWidth;
}

int Decoder::videoHeight() const
{
	return m_detail->videoHeight;
}

void Decoder::initLibavcodec()
{
	static std::once_flag initAVFlag;
	std::call_once(initAVFlag, []() {
		av_register_all();
	});

}

void Decoder::findFirstVideoStream()
{
	for (int streamIdx = 0; streamIdx < m_detail->format->nb_streams; ++streamIdx) {
		AVStream *stream = m_detail->format->streams[streamIdx];
		if (stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			m_detail->videoStreamIdx = streamIdx;
			return;
		}
	}

	throw std::runtime_error("Didn't find any video stream in the file (probably audio file)");
}

void Decoder::openCodec()
{
	AVStream *stream = m_detail->format->streams[m_detail->videoStreamIdx];
	AVCodecContext *pCodecCtx = stream->codec;

	AVCodec *codec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (!codec)
		throw std::runtime_error("Codec required by video file not available");

	if (avcodec_open2(pCodecCtx, codec, nullptr) < 0)
		throw std::runtime_error("Could not open codec");

	m_detail->codecCtx = std::shared_ptr<AVCodecContext>(pCodecCtx, &avcodec_close);
	m_detail->videoWidth = pCodecCtx->width;
	m_detail->videoHeight = pCodecCtx->height;
}

void Decoder::prepareTargetBuffer()
{
	m_detail->currentFrame = std::shared_ptr<AVFrame>(av_frame_alloc(), &av_free);
}

void Decoder::prepareResizeContext()
{
	m_detail->swsCtx = sws_getContext(m_detail->videoWidth,
									  m_detail->videoHeight,
									  m_detail->codecCtx->pix_fmt,
									  m_detail->videoWidth,
									  m_detail->videoHeight,
									  PIX_FMT_BGR24,
									  SWS_BILINEAR,
									  nullptr, nullptr, nullptr);

	if (!m_detail->swsCtx)
		throw std::runtime_error("Cannot get sws_getContext for resizing");

	AVPicture frameAsBGR24;
	avpicture_fill(&frameAsBGR24, nullptr, PIX_FMT_BGR24, m_detail->videoWidth, m_detail->videoHeight);
	memcpy(m_detail->BGR24Linesize, frameAsBGR24.linesize, sizeof(m_detail->BGR24Linesize));
}

bool Decoder::grab()
{
	SafeAVPacket packet;
	int isFrameAvailable;

	while (packet.reset(m_detail->format.get()) >= 0) {
		if (packet.packet.stream_index != m_detail->videoStreamIdx)
			continue;

		avcodec_decode_video2(m_detail->codecCtx.get(), m_detail->currentFrame.get(), &isFrameAvailable, &packet.packet);

		// ERROR, ERROR, FIXME!!!!!!!!!!!!!!!!
		// I wrongly assume a packet fills whole frame
		if (isFrameAvailable)
			return true;
	}

	return false;
}

bool Decoder::readBGR24Frame(uint8_t *dest, int *linesize)
{
	if (!dest || !linesize)
		return false;

	uint8_t *const destA[1] = {dest};
	sws_scale(m_detail->swsCtx,
			  (uint8_t const * const *)m_detail->currentFrame->data,
			  m_detail->currentFrame->linesize,
			  0,
			  m_detail->videoHeight,
			  destA,
			  m_detail->BGR24Linesize);

	*linesize = m_detail->BGR24Linesize[0];

	return true;
}