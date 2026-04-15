#include "rtspserver/media/MediaSource.hpp"
// AnnexBParser included transitively via IMediaSource.hpp

#include "rtspserver/utils/Logger.hpp"
#include "rtspserver/utils/StringUtils.hpp"

extern "C" {
#include <libavutil/opt.h>
}

#include <cstring>

using namespace rtspserver::sdp; // H264StreamInfo, AudioStreamInfo
using namespace rtspserver::utils::StringUtils;

namespace rtspserver::media {

MediaSource::~MediaSource() { close(); }

void MediaSource::close()
{
    audio_pkt_.reset();
    audio_queue_.clear();
    pkt_.reset();
    bsf_ctx_.reset();
    fmt_ctx_.reset();
    video_idx_ = -1;
    audio_idx_ = -1;
    first_pts_ = AV_NOPTS_VALUE;
    audio_first_pts_ = AV_NOPTS_VALUE;
    rtp_offset_ = 0;
    audio_rtp_offset_ = 0;
    audio_info_ = {};
    bsf_at_eof_ = false;
}

bool MediaSource::open(const std::string& path)
{
    close();

    // Open container
    AVFormatContext* raw_fmt = nullptr;
    if (avformat_open_input(&raw_fmt, path.c_str(), nullptr, nullptr) < 0) {
        LOG_ERROR("MediaSource: cannot open '", path, "'");
        return false;
    }
    fmt_ctx_.reset(raw_fmt);

    if (avformat_find_stream_info(fmt_ctx_.get(), nullptr) < 0) {
        LOG_WARN("MediaSource: could not determine stream info for '", path, "'");
    }

    // Find the best (first) H.264 video stream
    video_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO,
        -1, -1, nullptr, 0);
    if (video_idx_ < 0) {
        LOG_ERROR("MediaSource: no video stream in '", path, "'");
        fmt_ctx_.reset();
        return false;
    }

    FFmpegHandle::StreamView vst { fmt_ctx_->streams[video_idx_] };
    if (vst.codecpar()->codec_id != AV_CODEC_ID_H264) {
        LOG_ERROR("MediaSource: video codec is not H.264 (codec_id=",
            vst.codecpar()->codec_id, "). Only H.264 is supported.");
        fmt_ctx_.reset();
        return false;
    }

    time_base_ = vst.timeBase();

    // Set stream_info dimensions / frame rate / duration
    stream_info_.width = vst.codecpar()->width;
    stream_info_.height = vst.codecpar()->height;
    if (vst.avgFrameRate().den != 0 && vst.avgFrameRate().num != 0) {
        stream_info_.frame_rate = static_cast<float>(
            av_q2d(vst.avgFrameRate()));
    }
    if (fmt_ctx_->duration != AV_NOPTS_VALUE && fmt_ctx_->duration > 0) {
        stream_info_.duration_secs = static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;
    }

    // Parse extradata → SPS + PPS
    if (!parseExtradata({ vst.codecpar()->extradata,
            static_cast<size_t>(vst.codecpar()->extradata_size) })) {
        LOG_WARN("MediaSource: could not extract SPS/PPS from extradata");
    }

    // If extradata is in AVCC format, install h264_mp4toannexb BSF so that
    // every packet we read is automatically converted to Annex-B.
    bool is_avcc = (vst.codecpar()->extradata_size >= 4 && vst.codecpar()->extradata[0] == 0x01);

    if (is_avcc) {
        bsf_ctx_ = FFmpegHandle::make_bsf_context("h264_mp4toannexb");
        if (!bsf_ctx_) {
            LOG_ERROR("MediaSource: h264_mp4toannexb BSF not available");
            fmt_ctx_.reset();
            return false;
        }
        avcodec_parameters_copy(bsf_ctx_->par_in, vst.codecpar());
        bsf_ctx_->time_base_in = vst.timeBase();
        if (av_bsf_init(bsf_ctx_.get()) < 0) {
            LOG_ERROR("MediaSource: failed to initialize h264_mp4toannexb BSF");
            bsf_ctx_.reset();
            fmt_ctx_.reset();
            return false;
        }
        LOG_DEBUG("MediaSource: h264_mp4toannexb BSF installed");
    }

    pkt_ = FFmpegHandle::make_packet();
    if (!pkt_) {
        LOG_ERROR("MediaSource: av_packet_alloc failed");
        close();
        return false;
    }

    // ── Find best audio stream (optional) ────────────────────────────────────
    audio_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_AUDIO,
        -1, -1, nullptr, 0);
    if (audio_idx_ >= 0) {
        FFmpegHandle::StreamView ast { fmt_ctx_->streams[audio_idx_] };
        audio_time_base_ = ast.timeBase();
        audio_info_.present = true;
        audio_info_.sample_rate = ast.codecpar()->sample_rate;
        audio_info_.channels = ast.codecpar()->ch_layout.nb_channels;

        // Copy MPEG-4 AudioSpecificConfig from extradata if present
        if (ast.codecpar()->extradata && ast.codecpar()->extradata_size >= 2) {
            audio_info_.asc.assign(ast.codecpar()->extradata,
                ast.codecpar()->extradata
                    + ast.codecpar()->extradata_size);
        }

        audio_pkt_ = FFmpegHandle::make_packet();
        if (!audio_pkt_) {
            LOG_WARN("MediaSource: failed to alloc audio packet; audio disabled");
            audio_idx_ = -1;
            audio_info_ = {};
        } else {
            LOG_INFO("MediaSource: audio stream found (",
                audio_info_.sample_rate, " Hz, ",
                audio_info_.channels, " ch)");
        }
    } else {
        LOG_DEBUG("MediaSource: no audio stream in '", path, "'");
        audio_idx_ = -1;
    }

    LOG_INFO("MediaSource: opened '", path,
        "' (", stream_info_.width, 'x', stream_info_.height,
        " @ ", stream_info_.frame_rate, " fps)");
    return true;
}

bool MediaSource::parseExtradata(std::span<const uint8_t> extra)
{
    const uint8_t* data = extra.data();
    int size = static_cast<int>(extra.size());
    if (size < 4)
        return false;

    if (data[0] == 0x01) {
        // AVCC format
        // configurationVersion(1) | profile_idc(1) | constraints(1) |
        // level_idc(1) | lengthSizeMinusOne(1) | numSPS(1) | ...
        if (size < 7)
            return false;

        int num_sps = data[5] & 0x1F;
        int offset = 6;

        for (int i = 0; i < num_sps; ++i) {
            if (offset + 2 > size)
                break;
            int len = (data[offset] << 8) | data[offset + 1];
            offset += 2;
            if (offset + len > size)
                break;
            if (i == 0)
                stream_info_.sps.assign(data + offset, data + offset + len);
            offset += len;
        }
        if (offset >= size)
            return false;

        int num_pps = data[offset++];
        for (int i = 0; i < num_pps; ++i) {
            if (offset + 2 > size)
                break;
            int len = (data[offset] << 8) | data[offset + 1];
            offset += 2;
            if (offset + len > size)
                break;
            if (i == 0)
                stream_info_.pps.assign(data + offset, data + offset + len);
            offset += len;
        }
    } else {
        // Annex-B format
        auto nals = AnnexBParser::split(extra);
        for (auto& nal : nals) {
            if (nal.empty())
                continue;
            uint8_t type = nal[0] & 0x1Fu;
            if (type == 7 && stream_info_.sps.empty())
                stream_info_.sps = nal;
            else if (type == 8 && stream_info_.pps.empty())
                stream_info_.pps = nal;
        }
    }

    return !stream_info_.sps.empty() && !stream_info_.pps.empty();
}

uint32_t MediaSource::toRTPTimestamp(int64_t pts) const
{
    // Convert pts from stream time_base units to 90 kHz units
    int64_t ts = av_rescale_q(pts,
        time_base_,
        { 1, 90000 });
    return static_cast<uint32_t>(ts) + rtp_offset_;
}

double MediaSource::duration() const
{
    if (!fmt_ctx_ || fmt_ctx_->duration == AV_NOPTS_VALUE)
        return 0.0;
    return static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;
}

bool MediaSource::readPacket(VideoPacket& out)
{
    if (prefetch_vp_) {
        out = std::move(*prefetch_vp_);
        prefetch_vp_.reset();
        return true;
    }
    if (!pkt_)
        return false;

    // Called once we have a valid, Annex-B packet in pkt_.
    // Zero-copy: clones the AVPacket (increments its buffer ref-count), stores
    // the clone in out.pkt_data to keep the data alive, then builds NAL spans
    // pointing directly into that buffer.  A single av_packet_clone call
    // replaces the previous N per-NAL vector copies.
    auto processPacket = [&]() -> bool {
        out.is_key_frame = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
        out.duration = pkt_->duration;

        int64_t pts = (pkt_->pts != AV_NOPTS_VALUE) ? pkt_->pts : pkt_->dts;
        if (first_pts_ == AV_NOPTS_VALUE)
            first_pts_ = pts;

        out.rtp_timestamp = toRTPTimestamp(pts - first_pts_);

        // av_packet_clone is O(1): it allocates a new AVPacket header and
        // increments the reference count on the data buffer (no memcpy).
        out.pkt_data.reset(av_packet_clone(pkt_.get()));
        if (!out.pkt_data)
            return false;

        // Build span views into the retained buffer — no data copies.
        out.nals = AnnexBParser::splitView(out.pkt_data->data,
            out.pkt_data->size);

        // Release MediaSource's reference; the buffer lives on via out.pkt_data.
        av_packet_unref(pkt_.get());
        return true;
    };

    while (true) {
        // If we have a BSF, try to pull the next filtered packet first.
        if (bsf_ctx_) {
            int ret = av_bsf_receive_packet(bsf_ctx_.get(), pkt_.get());
            if (ret == 0)
                return processPacket();
            if (ret == AVERROR_EOF)
                return false; // BSF fully drained
            if (ret != AVERROR(EAGAIN))
                return false; // unexpected error
            // EAGAIN: BSF needs more input.
            // If we already flushed (container EOF), nothing more will come.
            if (bsf_at_eof_)
                return false;
        }

        // Read next packet from the container.
        int ret = av_read_frame(fmt_ctx_.get(), pkt_.get());
        if (ret < 0) {
            if (!bsf_ctx_)
                return false; // no BSF: true EOF
            // Flush the BSF so it releases any internally buffered packets
            // (e.g. B-frames held for reorder).  Loop back to drain them via
            // av_bsf_receive_packet before returning false.
            av_bsf_send_packet(bsf_ctx_.get(), nullptr);
            bsf_at_eof_ = true;
            continue;
        }

        if (pkt_->stream_index != video_idx_) {
            // Collect audio packets into the queue as a side-effect so that
            // readAudioPacket() can drain them without advancing the demuxer.
            if (audio_idx_ >= 0 && pkt_->stream_index == audio_idx_) {
                int64_t pts = (pkt_->pts != AV_NOPTS_VALUE) ? pkt_->pts : pkt_->dts;
                if (audio_first_pts_ == AV_NOPTS_VALUE)
                    audio_first_pts_ = pts;
                int64_t ts = av_rescale_q(pts - audio_first_pts_,
                    audio_time_base_,
                    { 1, audio_info_.sample_rate });
                AudioPacket ap;
                ap.rtp_timestamp = static_cast<uint32_t>(ts) + audio_rtp_offset_;
                ap.data.assign(pkt_->data, pkt_->data + pkt_->size);
                audio_queue_.push_back(std::move(ap));
            }
            av_packet_unref(pkt_.get());
            continue;
        }

        if (bsf_ctx_) {
            ret = av_bsf_send_packet(bsf_ctx_.get(), pkt_.get());
            av_packet_unref(pkt_.get()); // BSF owns the packet now
            if (ret < 0)
                return false;
            continue; // pull filtered output at the top of the loop
        }

        return processPacket();
    }
}

bool MediaSource::readAudioPacket(AudioPacket& out)
{
    if (audio_idx_ < 0)
        return false;
    if (audio_queue_.empty())
        return false;
    out = std::move(audio_queue_.front());
    audio_queue_.pop_front();
    return true;
}

void MediaSource::seek(uint32_t rtp_offset)
{
    if (!fmt_ctx_)
        return;

    // Flush BSF
    if (bsf_ctx_) {
        av_bsf_flush(bsf_ctx_.get());
    }

    av_seek_frame(fmt_ctx_.get(), video_idx_, 0, AVSEEK_FLAG_BACKWARD);
    rtp_offset_ = rtp_offset;
    first_pts_ = AV_NOPTS_VALUE;
    bsf_at_eof_ = false;
    // Advance audio RTP offset by the same wall-clock duration as video so
    // audio timestamps stay monotonically increasing across loops.
    // rtp_offset is in 90 kHz units; convert to audio sample-rate units.
    audio_first_pts_ = AV_NOPTS_VALUE;
    if (audio_info_.present && audio_info_.sample_rate > 0)
        audio_rtp_offset_ = static_cast<uint32_t>(
            static_cast<double>(rtp_offset) / 90000.0 * audio_info_.sample_rate);
    else
        audio_rtp_offset_ = 0;
    audio_queue_.clear();
    LOG_DEBUG("MediaSource: seeked to beginning (rtp_offset=", rtp_offset, ")");
}

double MediaSource::seekToSeconds(double position_secs)
{
    if (!fmt_ctx_ || position_secs < 0.0) {
        seek(0);
        return 0.0;
    }

    if (bsf_ctx_)
        av_bsf_flush(bsf_ctx_.get());

    FFmpegHandle::StreamView vst { fmt_ctx_->streams[video_idx_] };

    // Convert seconds to the stream's time_base units
    int64_t target_ts = av_rescale_q(
        static_cast<int64_t>(position_secs * AV_TIME_BASE),
        { 1, AV_TIME_BASE },
        vst.timeBase());

    // AVSEEK_FLAG_BACKWARD: seek to the nearest keyframe at or before target
    int ret = av_seek_frame(fmt_ctx_.get(), video_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        LOG_WARN("MediaSource: seekToSeconds(", position_secs, ") failed, rewinding to start");
        av_seek_frame(fmt_ctx_.get(), video_idx_, 0, AVSEEK_FLAG_BACKWARD);
        if (bsf_ctx_)
            av_bsf_flush(bsf_ctx_.get());
        bsf_at_eof_ = false;
        rtp_offset_ = 0;
        first_pts_ = AV_NOPTS_VALUE;
        return 0.0;
    }

    // av_seek_frame with AVSEEK_FLAG_BACKWARD lands on the nearest IDR keyframe
    // *before* the requested position – e.g. requesting 5.275 s may land on
    // the IDR at 4.0 s.  If we anchor rtp_offset_ to the *requested* time we
    // introduce a (4.0 → 5.275) = 1.275 s shift in every subsequent RTP
    // timestamp, making VLC's NPT display and jitter-buffer window wrong by
    // exactly that gap.
    //
    // Probe: read one raw video packet to learn the actual keyframe PTS, then
    // re-seek back to that exact position so readPacket() starts cleanly.
    // This costs two seeks (infrequent user-initiated operations) but is
    // correct for any keyframe interval and any FFmpeg version.
    int64_t actual_pts = target_ts; // fallback if probe fails
    {
        auto tmp = FFmpegHandle::make_packet();
        for (int skipped = 0; skipped < 256; ++skipped) {
            if (av_read_frame(fmt_ctx_.get(), tmp.get()) < 0)
                break;
            if (tmp->stream_index == video_idx_) {
                actual_pts = (tmp->pts != AV_NOPTS_VALUE) ? tmp->pts : tmp->dts;
                av_packet_unref(tmp.get());
                break;
            }
            av_packet_unref(tmp.get());
        }
        // tmp freed automatically by PacketPtr destructor.
        // Re-seek to the discovered keyframe so the decoder sees the full IDR.
        av_seek_frame(fmt_ctx_.get(), video_idx_, actual_pts, AVSEEK_FLAG_BACKWARD);
        if (bsf_ctx_)
            av_bsf_flush(bsf_ctx_.get());
    }
    bsf_at_eof_ = false;

    double actual_secs = actual_pts * av_q2d(vst.timeBase());
    rtp_offset_ = static_cast<uint32_t>(av_rescale_q(actual_pts, vst.timeBase(), { 1, 90000 }));
    // Do NOT pre-seed first_pts_.  The re-seek with AVSEEK_FLAG_BACKWARD may
    // land on the IDR just BEFORE actual_pts rather than on it. If first_pts_
    // were pre-seeded to actual_pts while the demuxer is one IDR earlier,
    // processPacket() would compute (earlier_pts - actual_pts) which is
    // negative, wrapping wildly when cast to uint32_t and producing a garbage
    // RTP timestamp.  Leaving it as AV_NOPTS_VALUE lets readPacket() anchor it
    // to the real first packet; rtp_offset_ still places the RTP clock at the
    // correct absolute position regardless.
    first_pts_ = AV_NOPTS_VALUE;
    // Reset audio anchor to the same seek position
    audio_first_pts_ = AV_NOPTS_VALUE;
    audio_rtp_offset_ = static_cast<uint32_t>(actual_secs * audio_info_.sample_rate);
    audio_queue_.clear();
    LOG_DEBUG("MediaSource: seeked to ", actual_secs, "s (requested ", position_secs,
        "s, rtp_offset=", rtp_offset_, ")");
    return actual_secs;
}

void MediaSource::adjustRTPOffset(uint32_t delta)
{
    rtp_offset_ += delta;
    if (audio_info_.present && audio_info_.sample_rate > 0) {
        const uint32_t audio_delta = static_cast<uint32_t>(
            static_cast<double>(delta) / 90000.0 * audio_info_.sample_rate);
        audio_rtp_offset_ += audio_delta;
    }
}

uint32_t MediaSource::peekNextVideoRTPTimestamp()
{
    if (!fmt_ctx_)
        return 0;
    if (prefetch_vp_)
        return prefetch_vp_->rtp_timestamp;
    VideoPacket vp;
    if (!readPacket(vp))
        return 0;
    const uint32_t ts = vp.rtp_timestamp;
    prefetch_vp_ = std::move(vp);
    return ts;
}

} // namespace rtspserver::media
