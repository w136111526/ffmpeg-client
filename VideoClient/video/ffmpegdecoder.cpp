﻿#include "ffmpegdecoder.h"
#include <limits.h>
#include <stdint.h>

#include "makeguard.h"
#include "interlockedadd.h"

#include <boost/chrono.hpp>
#include <utility>
#include <algorithm>

#include <boost/log/trivial.hpp>
#include "boost/filesystem.hpp"
using namespace boost::filesystem;

#define USE_HWACCEL

// http://stackoverflow.com/questions/34602561
#ifdef USE_HWACCEL
#include "ffmpeg_dxva2.h"
#endif

namespace
{

void FreeVideoCodecContext(AVCodecContext*& videoCodecContext)
{
#ifdef USE_HWACCEL
    if (videoCodecContext)
    {
        delete (InputStream*)videoCodecContext->opaque;
        videoCodecContext->opaque = nullptr;
    }
#endif

    // Close the codec
    avcodec_free_context(&videoCodecContext);
}

#ifdef USE_HWACCEL
AVPixelFormat GetHwFormat(AVCodecContext *s, const AVPixelFormat *pix_fmts)
{
    InputStream* ist = (InputStream*)s->opaque;
    ist->active_hwaccel_id = HWACCEL_DXVA2;
    ist->hwaccel_pix_fmt = AV_PIX_FMT_DXVA2_VLD;
    return ist->hwaccel_pix_fmt;
}
#endif

inline void Shutdown(const std::unique_ptr<boost::thread>& th)
{
    if (th)
    {
        th->interrupt();
        th->join();
    }
}

}  // namespace

namespace channel_logger
{

using boost::log::keywords::channel;

boost::log::sources::channel_logger_mt<> 
    ffmpeg_audio(channel = "ffmpeg_audio"),
    ffmpeg_closing(channel = "ffmpeg_closing"),
    ffmpeg_opening(channel = "ffmpeg_opening"),
    ffmpeg_pause(channel = "ffmpeg_pause"),
    ffmpeg_readpacket(channel = "ffmpeg_readpacket"),
    ffmpeg_seek(channel = "ffmpeg_seek"),
    ffmpeg_sync(channel = "ffmpeg_sync"),
    ffmpeg_threads(channel = "ffmpeg_threads"),
    ffmpeg_volume(channel = "ffmpeg_volume");

} // namespace channel_logger

double GetHiResTime()
{
    return boost::chrono::duration_cast<boost::chrono::microseconds>(
               boost::chrono::high_resolution_clock::now().time_since_epoch())
               .count() /
           1000000.;
}

std::unique_ptr<IFrameDecoder> GetFrameDecoder()
{
    return std::unique_ptr<IFrameDecoder>(new FFmpegDecoder());
}

// https://gist.github.com/xlphs/9895065
class FFmpegDecoder::IOContext
{
private:
    AVIOContext *ioCtx;
    uint8_t *buffer;  // internal buffer for ffmpeg
    int bufferSize;
    FILE *fh;

public:
    IOContext(const PathType &datafile);
    ~IOContext();

    void initAVFormatContext(AVFormatContext *);

    bool valid() const { return fh != nullptr; }

    static int IOReadFunc(void *data, uint8_t *buf, int buf_size);
    static int64_t IOSeekFunc(void *data, int64_t pos, int whence);
};

// static
int FFmpegDecoder::IOContext::IOReadFunc(void *data, uint8_t *buf, int buf_size)
{
    IOContext *hctx = (IOContext *)data;
    size_t len = fread(buf, 1, buf_size, hctx->fh);
    if (len == 0)
    {
        // Let FFmpeg know that we have reached EOF, or do something else
        return AVERROR_EOF;
    }
    return (int)len;
}

// whence: SEEK_SET, SEEK_CUR, SEEK_END (like fseek) and AVSEEK_SIZE
// static
int64_t FFmpegDecoder::IOContext::IOSeekFunc(void *data, int64_t pos, int whence)
{
    IOContext *hctx = (IOContext *)data;

    if (whence == AVSEEK_SIZE)
    {
        // return the file size if you wish to
        auto current = _ftelli64(hctx->fh);
        int rs = _fseeki64(hctx->fh, 0, SEEK_END);
        if (rs != 0)
        {
            return -1LL;
        }
        int64_t result = _ftelli64(hctx->fh);
        _fseeki64(hctx->fh, current, SEEK_SET);  // reset to the saved position
        return result;
    }

    int rs = _fseeki64(hctx->fh, pos, whence);
    if (rs != 0)
    {
        return -1LL;
    }
    return _ftelli64(hctx->fh);  // int64_t is usually long long
}

FFmpegDecoder::IOContext::IOContext(const PathType &s)
{
    // allocate buffer
    bufferSize = 1024 * 64;                     // FIXME: not sure what size to use
    buffer = (uint8_t *)av_malloc(bufferSize);  // see destructor for details

                                                // open file
    if (!(fh = 
#ifdef _WIN32
        _fsopen(s.c_str(), "rb", _SH_DENYNO)
#else
        _fsopen(s.c_str(), "rb", _SH_DENYNO)
#endif
    ))
    {
        // fprintf(stderr, "MyIOContext: failed to open file %s\n", s.c_str());
        BOOST_LOG_TRIVIAL(error) << "MyIOContext: failed to open file";
    }

    // allocate the AVIOContext
    ioCtx =
        avio_alloc_context(buffer, bufferSize,  // internal buffer and its size
            0,                   // write flag (1=true,0=false)
            (void *)this,  // user data, will be passed to our callback functions
            IOReadFunc,
            0,  // no writing
            IOSeekFunc);
}

FFmpegDecoder::IOContext::~IOContext()
{
    if (fh)
        fclose(fh);

    // NOTE: ffmpeg messes up the buffer
    // so free the buffer first then free the context
    av_free(ioCtx->buffer);
    ioCtx->buffer = nullptr;
    av_free(ioCtx);
}

void FFmpegDecoder::IOContext::initAVFormatContext(AVFormatContext *pCtx)
{
    pCtx->pb = ioCtx;
    pCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // you can specify a format directly
    // pCtx->iformat = av_find_input_format("h264");

    // or read some of the file and let ffmpeg do the guessing
    size_t len = fread(buffer, 1, bufferSize, fh);
    if (len == 0)
        return;
    _fseeki64(fh, 0, SEEK_SET);  // reset to beginning of file

    AVProbeData probeData = { 0 };
    probeData.buf = buffer;
    probeData.buf_size = bufferSize - 1;
    probeData.filename = "";
    pCtx->iformat = av_probe_input_format(&probeData, 1);
}

//////////////////////////////////////////////////////////////////////////////
void FFmpegDecoder::WriteErrorInfo(const char* format, ...)
{
	//return;
	const int INFOFILELEN = 5 * 1024 * 1024;
	char   g_szLogFileName[260] = { 0 };
	strcat(g_szLogFileName, "C:\\FFmpegDecoder.txt");
	FILE *fp;
	if ((fp = fopen(g_szLogFileName, "at")) == NULL)
		return;

	if (_filelength(_fileno(fp)) > INFOFILELEN)
	{
		fclose(fp);
		if ((fp = fopen(g_szLogFileName, "wt")) == NULL) return;
	}

	char buf[200];
	_strdate(buf);
	fprintf(fp, "%s ", buf);            //写日期
	_strtime(buf);
	fprintf(fp, "%s ", buf);            //写时间
	va_list pvar;
	va_start(pvar, format);
	vsprintf(buf, (char *)format, pvar);
	fprintf(fp, "%s\n", buf);
	va_end(pvar);

	fclose(fp);
}

FFmpegDecoder::FFmpegDecoder()
    : m_frameListener(nullptr),
      m_decoderListener(nullptr),
      m_pixelFormat(AV_PIX_FMT_YUV420P),
      m_allowDirect3dData(false),
	  m_bIsFile(false),
	  m_bValidDxva2(false),
	  m_bIsCamera(false),
	  m_bDesktop(false),
	  m_bLoopEnable(false)
{
 
    resetVariables();
    // init codecs
    avcodec_register_all();
    av_register_all();

    //avdevice_register_all();
    avformat_network_init();
}

FFmpegDecoder::~FFmpegDecoder() { close(); }

void FFmpegDecoder::resetVariables()
{
    m_videoCodec = nullptr;
    m_formatContext = nullptr;
    m_videoCodecContext = nullptr;
    m_videoFrame = nullptr;
    m_videoStream = nullptr;

    m_startTime = 0;
    m_currentTime = 0;
    m_duration = 0;

    m_imageCovertContext = nullptr;

    m_audioPTS = 0;

    m_frameDisplayingRequested = false;

    m_generation = 0;

    m_isPaused = false;

	m_bValidHardWare = false;

    m_seekDuration = AV_NOPTS_VALUE;
    m_videoResetDuration = AV_NOPTS_VALUE;

    m_videoResetting = false;

    m_isVideoSeekingWhilePaused = false;

    m_isPlaying = false;

    CHANNEL_LOG(ffmpeg_closing) << "Variables reset";
}

void FFmpegDecoder::close()
{
    CHANNEL_LOG(ffmpeg_closing) << "Start file closing";

    CHANNEL_LOG(ffmpeg_closing) << "Aborting threads";
    Shutdown(m_mainParseThread);  // controls other threads, hence stop first
    Shutdown(m_mainVideoThread);
    Shutdown(m_mainDisplayThread);

    closeProcessing();

    if (m_decoderListener)
        m_decoderListener->playingFinished();
}

void FFmpegDecoder::closeProcessing()
{
    m_videoPacketsQueue.clear();

    CHANNEL_LOG(ffmpeg_closing) << "Closing old vars";

    m_mainVideoThread.reset();
    m_mainParseThread.reset();
    m_mainDisplayThread.reset();

    // Free videoFrames
    m_videoFramesQueue.clear();

    sws_freeContext(m_imageCovertContext);

    // Free the YUV frame
    av_frame_free(&m_videoFrame);

    FreeVideoCodecContext(m_videoCodecContext);

    bool isFileReallyClosed = false;

    // Close video file
    if (m_formatContext)
    {
        avformat_close_input(&m_formatContext);
        isFileReallyClosed = true;
    }

    m_ioCtx.reset();

    CHANNEL_LOG(ffmpeg_closing) << "Old file closed";

    resetVariables();

    if (isFileReallyClosed)
    {
        CHANNEL_LOG(ffmpeg_closing) << "File was opened. Emit file closing signal";
        if (m_decoderListener)
            m_decoderListener->fileReleased();
    }

    if (m_decoderListener)
        m_decoderListener->decoderClosed();
}

bool FFmpegDecoder::openFile(const PathType& filename)
{
	return openDecoder(filename, std::string(), true);
}

bool FFmpegDecoder::openUrl(const std::string& url)
{
	return openDecoder(PathType(), url, false);
}

bool FFmpegDecoder::openCamera()
{
	return openDecoder(PathType(), std::string(), false, true, false);
}

bool FFmpegDecoder::openDesktop()
{
	return openDecoder(PathType(), std::string(), false, false, true);
}

void FFmpegDecoder::SetLoopEnable(bool bLoop)
{
	m_bLoopEnable = bLoop;
}

bool FFmpegDecoder::openDecoder(const PathType &file, const std::string& url, bool isFile, bool bCamera, bool bDesktop)
{
	m_bIsFile = isFile;
	WriteErrorInfo("Start Open Video File(%s%s)", url.c_str(), file.c_str());
    std::unique_ptr<IOContext> ioCtx;
    if (isFile)
    {
		path p(file);
		if (false == boost::filesystem::is_regular_file(p))
			return false;
        ioCtx.reset(new IOContext(file));
        if (!ioCtx->valid())
        {
            BOOST_LOG_TRIVIAL(error) << "Couldn't open video/audio file";
            return false;
        }
    }

    AVDictionary *streamOpts = nullptr;
    auto avOptionsGuard = MakeGuard(&streamOpts, av_dict_free);

    m_formatContext = avformat_alloc_context();
    if (isFile)
    {
        ioCtx->initAVFormatContext(m_formatContext);
    }
    else
    {
        av_dict_set(&streamOpts, "stimeout", "5000000", 0); // 5 seconds timeout.
    }

    auto formatContextGuard = MakeGuard(&m_formatContext, avformat_close_input);

    // Open video file
	AVInputFormat* ifmt = NULL;
	int error = 0;
	if (true == bCamera && false == bDesktop)
	{
		ifmt = av_find_input_format("vfwcap");
		if (nullptr != ifmt)
			error = avformat_open_input(&m_formatContext, nullptr, ifmt, nullptr);
		else
		{
			BOOST_LOG_TRIVIAL(error) << "Couldn't open camera error: " << error;
			return false;
		}
	}
	else if (false == bCamera && true == bDesktop)
	{
		ifmt = av_find_input_format("gdigrab");
		if (nullptr != ifmt)
			error = avformat_open_input(&m_formatContext, "desktop", ifmt, nullptr);
		else
		{
			BOOST_LOG_TRIVIAL(error) << "Couldn't open camera error: " << error;
			return false;
		}
	}
	else
		error = avformat_open_input(&m_formatContext, url.c_str(), nullptr, &streamOpts);

	if (error != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "Couldn't open video/audio file error: " << error;
		return false;
	}
	CHANNEL_LOG(ffmpeg_opening) << "Opening video/audio file...";

    // Retrieve stream information
    if (avformat_find_stream_info(m_formatContext, nullptr) < 0)
    {
        CHANNEL_LOG(ffmpeg_opening) << "Couldn't find stream information";
        return false;
    }

    // Find the first video stream
    m_videoStreamNumber = -1;
    for (unsigned i = m_formatContext->nb_streams; i--;)
    {
        switch (m_formatContext->streams[i]->codecpar->codec_type)
        {
        case AVMEDIA_TYPE_VIDEO:
            m_videoStream = m_formatContext->streams[i];
            m_videoStreamNumber = i;
            break;
        case AVMEDIA_TYPE_AUDIO:
            break;
        }
    }
    AVStream* timeStream = nullptr;

    if (m_videoStreamNumber == -1)
    {
        CHANNEL_LOG(ffmpeg_opening) << "Can't find video stream";
    }
    else
    {
        timeStream = m_videoStream;
    }

    m_startTime = (timeStream->start_time > 0)
        ? timeStream->start_time
        : ((m_formatContext->start_time == AV_NOPTS_VALUE)? 0 
			: int64_t((m_formatContext->start_time / av_q2d(timeStream->time_base)) / 1000000LL));
    m_duration = (timeStream->duration > 0)
        ? timeStream->duration
        : ((m_formatContext->duration == AV_NOPTS_VALUE)? 0 
			: int64_t((m_formatContext->duration / av_q2d(timeStream->time_base)) / 1000000LL));

	WriteErrorInfo("Reset Video Processing");
    if (!resetVideoProcessing())
    {
        return false;
    }
    m_videoFrame = av_frame_alloc();

    formatContextGuard.release();
    m_ioCtx = std::move(ioCtx);

    if (m_decoderListener)
    {
        m_decoderListener->fileLoaded();
        m_decoderListener->changedFramePosition(m_startTime, m_startTime, m_duration + m_startTime);
    }
	WriteErrorInfo("Open Video Success%s", url.c_str());

    return true;
}

bool FFmpegDecoder::resetVideoProcessing()
{
    FreeVideoCodecContext(m_videoCodecContext);

    auto videoCodecContextGuard = MakeGuard(&m_videoCodecContext, avcodec_free_context);

    // Find the decoder for the video stream
    if (m_videoStreamNumber >= 0)
    {
        CHANNEL_LOG(ffmpeg_opening) << "Video steam number: " << m_videoStreamNumber;
        m_videoCodecContext = avcodec_alloc_context3(nullptr);
        if (!m_videoCodecContext)
            return false;
        if (avcodec_parameters_to_context(m_videoCodecContext, m_videoStream->codecpar) < 0)
            return false;

        m_videoCodec = avcodec_find_decoder(m_videoCodecContext->codec_id);
        if (m_videoCodec == nullptr)
        {
            assert(false && "No such codec found");
            return false;  // Codec not found
        }

#ifdef USE_HWACCEL
        m_videoCodecContext->coded_width = m_videoCodecContext->width;
        m_videoCodecContext->coded_height = m_videoCodecContext->height;

        m_videoCodecContext->thread_count = 1;  // Multithreading is apparently not compatible with hardware decoding
        InputStream *ist = new InputStream();
        ist->hwaccel_id = HWACCEL_AUTO;
        ist->hwaccel_device = "dxva2";
        ist->dec = m_videoCodec;
        ist->dec_ctx = m_videoCodecContext;

        m_videoCodecContext->opaque = ist;
        if (dxva2_init(m_videoCodecContext) >= 0)
        {
            m_videoCodecContext->get_buffer2 = ist->hwaccel_get_buffer;
            m_videoCodecContext->get_format = GetHwFormat;
            m_videoCodecContext->thread_safe_callbacks = 1;
			m_bValidHardWare = true;
        }
        else
        {
            delete ist;
            m_videoCodecContext->opaque = nullptr;
            m_videoCodecContext->thread_count = 2;
            m_videoCodecContext->flags2 |= CODEC_FLAG2_FAST;
			m_bValidHardWare = false;
        }
#else
        m_videoCodecContext->thread_count = 2;
        m_videoCodecContext->flags2 |= CODEC_FLAG2_FAST;
#endif

        //m_videoCodecContext->refcounted_frames = 1;

    // Open codec
        if (avcodec_open2(m_videoCodecContext, m_videoCodec, nullptr) < 0)
        {
            assert(false && "Error on codec opening");
            return false;  // Could not open codec
        }

        // Some broken files can pass codec check but don't have width x height
        if (m_videoCodecContext->width <= 0 || m_videoCodecContext->height <= 0)
        {
            assert(false && "This file lacks resolution");
            return false;  // Could not open codec
        }
    }
    videoCodecContextGuard.release();

    return true;
}

void FFmpegDecoder::play(bool isPaused)
{
    CHANNEL_LOG(ffmpeg_opening) << "Starting playing";

    m_isPaused = isPaused;

    if (isPaused)
    {
        m_pauseTimer = GetHiResTime();
    }

    if (!m_mainParseThread)
    {
        m_isPlaying = true;
        m_mainParseThread.reset(new boost::thread(&FFmpegDecoder::parseRunnable, this));
        m_mainDisplayThread.reset(new boost::thread(&FFmpegDecoder::displayRunnable, this));
        CHANNEL_LOG(ffmpeg_opening) << "Playing";
    }
}

void FFmpegDecoder::SetFrameFormat(FrameFormat format, bool allowDirect3dData)
{ 
    static_assert(PIX_FMT_YUV420P == AV_PIX_FMT_YUV420P, "FrameFormat and AVPixelFormat values must coincide.");
    static_assert(PIX_FMT_YUYV422 == AV_PIX_FMT_YUYV422, "FrameFormat and AVPixelFormat values must coincide.");
    static_assert(PIX_FMT_RGB24 == AV_PIX_FMT_RGB24,     "FrameFormat and AVPixelFormat values must coincide.");

    m_pixelFormat = (AVPixelFormat)format;
    m_allowDirect3dData = allowDirect3dData;
}

void FFmpegDecoder::finishedDisplayingFrame(unsigned int generation)
{
    {
        boost::lock_guard<boost::mutex> locker(m_videoFramesMutex);
        if (generation == m_generation && m_videoFramesQueue.canPop())
        {
            VideoFrame &current_frame = m_videoFramesQueue.front();
            if (current_frame.m_image->format == AV_PIX_FMT_DXVA2_VLD)
            {
                av_frame_unref(current_frame.m_image);
            }

            m_videoFramesQueue.popFront();
        }
        m_frameDisplayingRequested = false;
    }
    m_videoFramesCV.notify_all();
}

bool FFmpegDecoder::seekDuration(int64_t duration)
{
	if (!m_bIsFile) return false;

    if (m_mainParseThread && m_seekDuration.exchange(duration) == AV_NOPTS_VALUE)
    {
        m_videoPacketsQueue.notify();
    }

    return true;
}

void FFmpegDecoder::videoReset()
{
    m_videoResetting = true;
    if (m_mainParseThread && m_videoResetDuration.exchange(m_currentTime) == AV_NOPTS_VALUE)
    {
        m_videoPacketsQueue.notify();
    }
}

void FFmpegDecoder::seekWhilePaused()
{
    const bool paused = m_isPaused;
    if (paused)
    {
        InterlockedAdd(m_videoStartClock, GetHiResTime() - m_pauseTimer);
        m_pauseTimer = GetHiResTime();
    }

    m_isVideoSeekingWhilePaused = paused;
}

bool FFmpegDecoder::seekByPercent(double percent)
{
    return seekDuration(m_startTime + int64_t(m_duration * percent));
}

bool FFmpegDecoder::getFrameRenderingData(FrameRenderingData *data)
{
    if (!m_frameDisplayingRequested || m_mainParseThread == nullptr || m_videoResetting)
    {
        return false;
    }

    VideoFrame &current_frame = m_videoFramesQueue.front();
	if (nullptr == current_frame.pBGR || current_frame.m_nImageWidth == 0 || current_frame.m_nImageHeight == 0)
		return false;
    data->width = current_frame.m_nImageWidth;
    data->height = current_frame.m_nImageHeight;
	data->pBGR	= current_frame.pBGR;
    if (current_frame.m_image->sample_aspect_ratio.num != 0
        && current_frame.m_image->sample_aspect_ratio.den != 0)
    {
        data->aspectNum = current_frame.m_image->sample_aspect_ratio.num;
        data->aspectDen = current_frame.m_image->sample_aspect_ratio.den;
    }
    else
    {
        data->aspectNum = 1;
        data->aspectDen = 1;
    }

    return true;
}

void FFmpegDecoder::handleDirect3dData(AVFrame* videoFrame, VideoFrame& video)
{
    if (m_allowDirect3dData && videoFrame->format == AV_PIX_FMT_DXVA2_VLD)
    {
        dxva2_retrieve_data_call(m_videoCodecContext, videoFrame, video);
        assert(videoFrame->format != AV_PIX_FMT_DXVA2_VLD);
    }
}