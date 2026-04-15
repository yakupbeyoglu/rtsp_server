/**
 * @file MediaSource.hpp
 * @brief FFmpeg-backed implementation of IMediaSource.
 */

#pragma once

#include <deque>
#include <optional>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "rtspserver/media/IMediaSource.hpp"

namespace rtspserver::media {

/**
 * @brief Concrete media source backed by FFmpeg.
 *
 * Demuxes an H.264 + optional AAC container, applies the h264_mp4toannexb
 * bitstream filter when needed, and exposes per-frame NAL units via
 * zero-copy spans tied to the lifetime of each VideoPacket.
 */
class MediaSource : public IMediaSource {
public:
    MediaSource() = default;
    ~MediaSource() override;

    // ── IMediaSource ─────────────────────────────────────────────────────────

    [[nodiscard]] bool open(const std::string& path) override;
    bool isOpen() const override { return fmt_ctx_ != nullptr; }
    void close() override;

    const sdp::H264StreamInfo& streamInfo() const override { return stream_info_; }
    const sdp::AudioStreamInfo& audioInfo() const override { return audio_info_; }
    double duration() const override;

    bool readPacket(VideoPacket& out) override;
    bool readAudioPacket(AudioPacket& out) override;

    void seek(uint32_t rtp_offset = 0) override;
    double seekToSeconds(double position_secs) override;

    uint32_t rtpOffset() const override { return rtp_offset_; }
    uint32_t audioRTPOffset() const override { return audio_rtp_offset_; }
    void adjustRTPOffset(uint32_t delta) override;

    uint32_t peekNextVideoRTPTimestamp() override;

private:
    bool parseExtradata(std::span<const uint8_t> extra);
    uint32_t toRTPTimestamp(int64_t pts) const;

    // ── FFmpeg state ─────────────────────────────────────────────────────────

    FFmpegHandle::FormatCtxPtr fmt_ctx_;
    FFmpegHandle::BSFCtxPtr bsf_ctx_;
    FFmpegHandle::PacketPtr pkt_;

    int video_idx_ { -1 };
    AVRational time_base_ { 1, 90000 };

    sdp::H264StreamInfo stream_info_;
    sdp::AudioStreamInfo audio_info_;

    int audio_idx_ { -1 };
    AVRational audio_time_base_ { 1, 44100 };
    int64_t audio_first_pts_ { AV_NOPTS_VALUE };
    uint32_t audio_rtp_offset_ { 0 };

    FFmpegHandle::PacketPtr audio_pkt_;
    std::deque<AudioPacket> audio_queue_;

    uint32_t rtp_offset_ { 0 };
    int64_t first_pts_ { AV_NOPTS_VALUE };
    bool bsf_at_eof_ { false };

    std::optional<VideoPacket> prefetch_vp_;
};

} // namespace rtspserver::media
