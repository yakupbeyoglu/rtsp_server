#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}

#include <memory>

// RAII wrappers for FFmpeg opaque handle types.

namespace rtspserver::media::FFmpegHandle {

// Deleter functors

struct FormatCtxDeleter {
    void operator()(AVFormatContext* p) const noexcept
    {
        avformat_close_input(&p);
    }
};

struct BSFCtxDeleter {
    void operator()(AVBSFContext* p) const noexcept
    {
        av_bsf_free(&p);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept
    {
        av_packet_free(&p);
    }
};

using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using BSFCtxPtr = std::unique_ptr<AVBSFContext, BSFCtxDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

inline PacketPtr make_packet() noexcept
{
    return PacketPtr(av_packet_alloc());
}

/// Look up the named bitstream filter, allocate its context, and return an
/// owning BSFCtxPtr.  Returns nullptr if the filter is not found or
/// av_bsf_alloc fails — the raw `const AVBitStreamFilter*` never escapes.
inline BSFCtxPtr make_bsf_context(const char* filter_name) noexcept
{
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(filter_name);
    if (!bsf)
        return nullptr;
    AVBSFContext* ctx = nullptr;
    if (av_bsf_alloc(bsf, &ctx) < 0)
        return nullptr;
    return BSFCtxPtr(ctx);
}

/**
 * RAII wrapper for AVStream* that provides convenient accessors for commonly used fields.
 */
class StreamView {
public:
    explicit StreamView(AVStream* st) noexcept
        : st_(st)
    {
    }

    bool valid() const noexcept { return st_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    // Raw access for FFmpeg APIs that require AVStream* directly.
    AVStream* get() const noexcept { return st_; }

    // Commonly accessed stream properties.
    AVRational timeBase() const noexcept { return st_->time_base; }
    AVRational avgFrameRate() const noexcept { return st_->avg_frame_rate; }
    AVCodecParameters* codecpar() const noexcept { return st_->codecpar; }

private:
    AVStream* st_ { nullptr };
};

} // namespace rtspserver::media::FFmpegHandle
