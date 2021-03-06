#include "ffmpeg.hh"

#include "config.hh"
#include "util.hh"
#include "xtime.hh"
#include <iostream>
#include <stdexcept>

extern "C" {
#include AVCODEC_INCLUDE
#include AVFORMAT_INCLUDE
#include SWSCALE_INCLUDE
}

#if (LIBAVCODEC_VERSION_INT) < (AV_VERSION_INT(52,94,3))
#	define AV_SAMPLE_FMT_S16 SAMPLE_FMT_S16
#endif

#define AUDIO_CHANNELS 2

/*static*/ boost::mutex FFmpeg::s_avcodec_mutex;

FFmpeg::FFmpeg(bool decodeVideo, bool decodeAudio, std::string const& _filename, unsigned int rate):
  width(), height(), m_filename(_filename), m_rate(rate), m_quit(), m_running(), m_eof(),
  m_seekTarget(getNaN()), m_position(), m_streamId(-1), m_mediaType(),
  m_formatContext(), m_codecContext(), m_codec(), m_resampleContext(), m_swsContext(),
  m_thread(new boost::thread(boost::ref(*this)))
{
	if (decodeVideo) m_mediaType = AVMEDIA_TYPE_VIDEO;
	else if (decodeAudio) m_mediaType = AVMEDIA_TYPE_AUDIO;
	else throw std::logic_error("Can only decode one track");
}

FFmpeg::~FFmpeg() {
	m_quit = true;
	videoQueue.reset();
	audioQueue.quit();
	m_thread->join();
	// TODO: use RAII for freeing resources (to prevent memory leaks)
	boost::mutex::scoped_lock l(s_avcodec_mutex); // avcodec_close is not thread-safe
	if (m_resampleContext) audio_resample_close(m_resampleContext);
	if (m_codecContext) avcodec_close(m_codecContext);
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(53, 17, 0)
	if (m_formatContext) avformat_close_input(&m_formatContext);
#else
	if (m_formatContext) av_close_input_file(m_formatContext);
#endif
}

double FFmpeg::duration() const {
	double d = m_running ? m_formatContext->duration / double(AV_TIME_BASE) : getNaN();
	return d >= 0.0 ? d : getInf();
}

void FFmpeg::open() {
	boost::mutex::scoped_lock l(s_avcodec_mutex);
	av_register_all();
	av_log_set_level(AV_LOG_ERROR);
	if (avformat_open_input(&m_formatContext, m_filename.c_str(), NULL, NULL)) throw std::runtime_error("Cannot open input file");
	if (avformat_find_stream_info(m_formatContext, NULL) < 0) throw std::runtime_error("Cannot find stream information");
	m_formatContext->flags |= AVFMT_FLAG_GENPTS;
	// Find a track and open the codec
	m_streamId = av_find_best_stream(m_formatContext, (AVMediaType)m_mediaType, -1, -1, &m_codec, 0);
	if (m_streamId < 0) throw std::runtime_error("No suitable track found");

	AVCodecContext* cc = m_formatContext->streams[m_streamId]->codec;
	if (avcodec_open2(cc, m_codec, NULL) < 0) throw std::runtime_error("Cannot open audio codec");
	cc->workaround_bugs = FF_BUG_AUTODETECT;
	m_codecContext = cc;

	switch (m_mediaType) {
	case AVMEDIA_TYPE_AUDIO:
		m_resampleContext = av_audio_resample_init(AUDIO_CHANNELS, cc->channels, m_rate, cc->sample_rate, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16, 16, 10, 0, 0.8);
		if (!m_resampleContext) throw std::runtime_error("Cannot create resampling context");
		audioQueue.setSamplesPerSecond(AUDIO_CHANNELS * m_rate);
		break;
	case AVMEDIA_TYPE_VIDEO:
		// Setup software scaling context for YUV to RGB conversion
		width = cc->width;
		height = cc->height;
		m_swsContext = sws_getContext(
		  cc->width, cc->height, cc->pix_fmt,
		  width, height, PIX_FMT_RGB24,
		  SWS_POINT, NULL, NULL, NULL);
		break;
	default:  // Should never be reached but avoids compile warnings
		break;
	}
}

void FFmpeg::operator()() {
	try { open(); } catch (std::exception const& e) { std::clog << "ffmpeg/error: Failed to open " << m_filename << ": " << e.what() << std::endl; m_quit = true; return; }
	m_running = true;
	audioQueue.setDuration(duration());
	int errors = 0;
	while (!m_quit) {
		try {
			if (audioQueue.wantSeek()) m_seekTarget = 0.0;
			if (m_seekTarget == m_seekTarget) seek_internal();
			decodePacket();
			m_eof = false;
			errors = 0;
		} catch (eof_error&) {
			m_eof = true;
			videoQueue.push(new VideoFrame()); // EOF marker
			boost::thread::sleep(now() + 0.1);
		} catch (std::exception& e) {
			std::clog << "ffmpeg/error: " << m_filename << ": " << e.what() << std::endl;
			if (++errors > 2) { std::clog << "ffmpeg/error: FFMPEG terminating due to multiple errors" << std::endl; m_quit = true; }
		}
	}
	m_running = false;
	m_eof = true;
	audioQueue.setEof();
	videoQueue.push(new VideoFrame()); // EOF marker
}

void FFmpeg::seek(double time, bool wait) {
	m_seekTarget = time;
	videoQueue.reset(); audioQueue.reset(); // Empty these to unblock the internals in case buffers were full
	if (wait) while (!m_quit && m_seekTarget == m_seekTarget) boost::thread::sleep(now() + 0.01);
}

void FFmpeg::seek_internal() {
	videoQueue.reset();
	audioQueue.reset();
	int flags = 0;
	if (m_seekTarget < position()) flags |= AVSEEK_FLAG_BACKWARD;
	av_seek_frame(m_formatContext, -1, m_seekTarget * AV_TIME_BASE, flags);
	m_seekTarget = getNaN(); // Signal that seeking is done
}

void FFmpeg::decodePacket() {
	struct ReadFramePacket: public AVPacket {
		AVFormatContext* m_s;
		ReadFramePacket(AVFormatContext* s): m_s(s) {
			if (av_read_frame(s, this) < 0) throw FFmpeg::eof_error();
		}
		~ReadFramePacket() { av_free_packet(this); }
	};

	struct AVFrameWrapper {
		AVFrame* m_frame;
		AVFrameWrapper(): m_frame(avcodec_alloc_frame()) {
			if (!m_frame) throw std::runtime_error("Unable to allocate AVFrame");
		}
		~AVFrameWrapper() { av_free(m_frame); }
		operator AVFrame*() { return m_frame; }
		AVFrame* operator->() { return m_frame; }
	};

	// Read an AVPacket and decode it into AVFrames
	ReadFramePacket packet(m_formatContext);
	int packetSize = packet.size;
	while (packetSize) {
		if (packetSize < 0) throw std::logic_error("negative packet size?!");
		if (m_quit || m_seekTarget == m_seekTarget) return;
		if (packet.stream_index != m_streamId) return;
		AVFrameWrapper frame;
		int frameFinished = 0;
		int decodeSize = (m_mediaType == AVMEDIA_TYPE_VIDEO ?
		  avcodec_decode_video2(m_codecContext, frame, &frameFinished, &packet) :
		  avcodec_decode_audio4(m_codecContext, frame, &frameFinished, &packet));
		if (decodeSize < 0) throw std::runtime_error("cannot decode avframe");
		packetSize -= decodeSize; // Move forward within the packet
		if (!frameFinished) continue;
		// Update current position if timecode is available
		if (frame->pkt_pts != uint64_t(AV_NOPTS_VALUE)) {
			m_position = double(frame->pkt_pts) * av_q2d(m_formatContext->streams[m_streamId]->time_base);
		}
		if (m_mediaType == AVMEDIA_TYPE_VIDEO) processVideo(frame); else processAudio(frame);
	}
}

void FFmpeg::processVideo(AVFrame* frame) {
	// Convert into RGB and scale the data
	int w = (m_codecContext->width+15)&~15;
	int h = m_codecContext->height;
	std::vector<uint8_t> buffer(w * h * 3);
	{
		uint8_t* data = &buffer[0];
		int linesize = w * 3;
		sws_scale(m_swsContext, frame->data, frame->linesize, 0, h, &data, &linesize);
	}
	// Construct a new video frame and push it to output queue
	VideoFrame* tmp = new VideoFrame(m_position, w, h);
	tmp->data.swap(buffer);
	videoQueue.push(tmp);  // Takes ownership and may block
}

void FFmpeg::processAudio(AVFrame* frame) {
	// Resample to output sample rate, then push to audio queue and increment timecode
	std::vector<int16_t> resampled(AVCODEC_MAX_AUDIO_FRAME_SIZE);
	int frames = audio_resample(m_resampleContext, &resampled[0], (short*)frame->data[0], frame->nb_samples);
	resampled.resize(frames * AUDIO_CHANNELS);
	audioQueue.push(resampled, m_position);  // May block
	m_position += double(frames)/m_formatContext->streams[m_streamId]->codec->sample_rate;
}

