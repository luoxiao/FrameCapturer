#include "pch.h"
#include "fcFoundation.h"
#include "fcI420.h"
#include "fcWebMFile.h"
#include "fcVorbisEncoder.h"
#include "fcVPXEncoder.h"
#include "fcWebMMuxer.h"
#include "GraphicsDevice/fcGraphicsDevice.h"

#ifdef _MSC_VER
    #pragma comment(lib, "vpxmt.lib")
    #pragma comment(lib, "libwebm.lib")
#endif // _MSC_VER



class fcWebMContext : public fcIWebMContext
{
public:
    fcWebMContext(fcWebMConfig &conf, fcIGraphicsDevice *gd);
    ~fcWebMContext() override;
    void release() override;

    void addOutputStream(fcStream *s) override;
    bool addVideoFrameTexture(void *tex, fcPixelFormat fmt, fcTime timestamp) override;
    bool addVideoFramePixels(const void *pixels, fcPixelFormat fmt, fcTime timestamp) override;
    bool addAudioFrame(const float *samples, int num_samples, fcTime timestamp) override;

    template<class Body>
    void eachStreams(const Body &b)
    {
        for (auto& s : m_muxers) { b(*s); }
    }

private:
    using VideoEncoderPtr = std::unique_ptr<fcIWebMVideoEncoder>;
    using AudioEncoderPtr = std::unique_ptr<fcIWebMAudioEncoder>;
    using MuxerPtr        = std::unique_ptr<fcIWebMMuxer>;
    using MuxerPtrs       = std::vector<MuxerPtr>;

    fcWebMConfig        m_conf;
    fcIGraphicsDevice   *m_gdev = nullptr;
    VideoEncoderPtr     m_video_encoder;
    AudioEncoderPtr     m_audio_encoder;
    MuxerPtrs           m_muxers;

    Buffer              m_texture_image;
    Buffer              m_rgba_image;
    fcI420Image         m_i420_image;
    fcWebMVideoFrame    m_video_frame;
    fcWebMAudioFrame    m_audio_frame;
};


fcWebMContext::fcWebMContext(fcWebMConfig &conf, fcIGraphicsDevice *gd)
    : m_conf(conf)
    , m_gdev(gd)
{
    if (conf.video) {
        fcVPXEncoderConfig econf;
        econf.width = conf.video_width;
        econf.height = conf.video_height;
        econf.target_bitrate = conf.video_bitrate;

        switch (conf.video_encoder) {
        case fcWebMVideoEncoder::VP8:
            m_video_encoder.reset(fcCreateVP8Encoder(econf));
            break;
        case fcWebMVideoEncoder::VP9:
            m_video_encoder.reset(fcCreateVP9Encoder(econf));
            break;
        }
    }

    if (conf.audio) {
        fcVorbisEncoderConfig econf;
        econf.sampling_rate = conf.audio_sample_rate;
        econf.num_channels = conf.audio_num_channels;
        econf.target_bitrate = conf.audio_bitrate;

        switch (conf.audio_encoder) {
        case fcWebMAudioEncoder::Vorbis:
            m_audio_encoder.reset(fcCreateVorbisEncoder(econf));
            break;
        case fcWebMAudioEncoder::Opus:
            m_audio_encoder.reset(fcCreateOpusEncoder(econf));
            break;
        }
    }
}

fcWebMContext::~fcWebMContext()
{
    m_video_encoder->flush(m_video_frame);
    m_video_encoder.reset();
    m_audio_encoder.reset();
    m_muxers.clear();
}

void fcWebMContext::release()
{
    delete this;
}

void fcWebMContext::addOutputStream(fcStream *s)
{
    auto *muxer = fcCreateWebMMuxer(*s, m_conf);
    if (m_video_encoder) { muxer->setVideoEncoderInfo(m_video_encoder->getMatroskaCodecID()); }
    if (m_audio_encoder) { muxer->setAudioEncoderInfo(m_audio_encoder->getMatroskaCodecID()); }
    m_muxers.emplace_back(muxer);
}

bool fcWebMContext::addVideoFrameTexture(void *tex, fcPixelFormat fmt, fcTime timestamp)
{
    if (!tex || !m_video_encoder || !m_gdev) { return false; }

    size_t psize = fcGetPixelSize(fmt);
    m_texture_image.resize(m_conf.video_width * m_conf.video_height * psize);
    if (!m_gdev->readTexture(m_texture_image.data(), m_texture_image.size(), tex, m_conf.video_width, m_conf.video_height, fmt))
    {
        return false;
    }

    addVideoFramePixels(m_texture_image.data(), fmt, timestamp);
    return true;
}

bool fcWebMContext::addVideoFramePixels(const void *pixels, fcPixelFormat fmt, fcTime timestamp)
{
    if (!pixels || !m_video_encoder) { return false; }

    fcI420Data i420;
    if (fmt == fcPixelFormat_I420) {
        int frame_size = m_conf.video_width * m_conf.video_height;
        i420.y = pixels;
        i420.u = (char*)i420.y + frame_size;
        i420.v = (char*)i420.u + (frame_size >> 2);
    }
    else if (fmt == fcPixelFormat_RGBAu8) {
        m_i420_image.resize(m_conf.video_width, m_conf.video_height);
        fcRGBA2I420(m_i420_image, pixels, m_conf.video_width, m_conf.video_height);
        i420 = m_i420_image.data();
    }
    else {
        m_rgba_image.resize(m_conf.video_width * m_conf.video_height * 4);
        fcConvertPixelFormat(m_rgba_image.data(), fcPixelFormat_RGBAu8, pixels, fmt, m_conf.video_width * m_conf.video_height);

        m_i420_image.resize(m_conf.video_width, m_conf.video_height);
        fcRGBA2I420(m_i420_image, m_rgba_image.data(), m_conf.video_width, m_conf.video_height);
        i420 = m_i420_image.data();
    }

    if (m_video_encoder->encode(m_video_frame, i420, timestamp)) {
        eachStreams([&](auto& muxer) {
            muxer.addVideoFrame(m_video_frame);
        });
        m_video_frame.clear();
    }
    return true;
}

bool fcWebMContext::addAudioFrame(const float *samples, int num_samples, fcTime timestamp)
{
    if (!samples || !m_audio_encoder) { return false; }

    if (m_audio_encoder->encode(m_audio_frame, samples, num_samples)) {
        eachStreams([&](auto& muxer) {
            muxer.addAudioFrame(m_audio_frame);
        });
        m_audio_frame.clear();
    }
    return true;
}

fcWebMAPI fcIWebMContext* fcWebMCreateContextImpl(fcWebMConfig &conf, fcIGraphicsDevice *gd)
{
    return new fcWebMContext(conf, gd);
}