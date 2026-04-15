/**
 * @file IMediaSource.hpp
 * @brief Runtime-polymorphic interface for media source implementations.
 *
 * Decouples the RTSP session layer (consumer) from the concrete FFmpeg-backed
 * MediaSource (provider), satisfying the Dependency Inversion Principle.
 * Mock or file-list implementations can be injected for unit testing.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rtspserver/media/AnnexBParser.hpp"
#include "rtspserver/media/FFmpegHandle.hpp"
#include "rtspserver/sdp/SDPBuilder.hpp"

namespace rtspserver::media {

/**
 * @describe Packet Types
 *  One decoded video frame, ready for RTP packetization.
 * Zero-copy: each span in `nals` points into `pkt_data`'s buffer —
 * `pkt_data` must be kept alive as long as `nals` is used, otherwise can be UB/crash.
 */
struct VideoPacket {
    types::NalUnitViews nals;
    FFmpegHandle::PacketPtr pkt_data; ///< Keeps the underlying FFmpeg buffer alive.
    uint32_t rtp_timestamp { 0 };
    bool is_key_frame { false };
    int64_t duration { 0 };
};

/// One encoded audio frame (AAC-LC, ADTS stripped if present).
struct AudioPacket {
    std::vector<uint8_t> data;
    uint32_t rtp_timestamp { 0 };
};

/**
 * @brief Abstract media source interface.
 *
 * All pure virtual — concrete implementations (e.g. FFmpeg-backed MediaSource,
 * or a test stub) must override every method.
 */
class IMediaSource {
public:
    virtual ~IMediaSource() = default;

    // Non-copyable
    IMediaSource(const IMediaSource&) = delete;
    IMediaSource& operator=(const IMediaSource&) = delete;

    /// Open the media at @p path. Returns false on failure.
    [[nodiscard]] virtual bool open(const std::string& path) = 0;

    /// Returns true if a source has been successfully opened.
    virtual bool isOpen() const = 0;

    /// Close and release all underlying resources.
    virtual void close() = 0;

    // meta data
    virtual const sdp::H264StreamInfo& streamInfo() const = 0;
    virtual const sdp::AudioStreamInfo& audioInfo() const = 0;

    /// Total duration in seconds (0 if unknown).
    virtual double duration() const = 0;

    /// Read the next video packet into @p out. Returns false at end-of-stream.
    virtual bool readPacket(VideoPacket& out) = 0;

    /// Dequeue the next buffered audio packet. Returns false if none available.
    virtual bool readAudioPacket(AudioPacket& out) = 0;

    /// Rewind to the beginning, applying @p rtp_offset as the new RTP base.
    virtual void seek(uint32_t rtp_offset = 0) = 0;

    /// Seek to the nearest keyframe at or before @p position_secs.
    /// Returns the actual position reached, in seconds.
    virtual double seekToSeconds(double position_secs) = 0;

    virtual uint32_t rtpOffset() const = 0;
    virtual uint32_t audioRTPOffset() const = 0;

    /// Shift both video and audio RTP clocks forward by @p delta ticks.
    virtual void adjustRTPOffset(uint32_t delta) = 0;

    /// Returns the RTP timestamp of the next video frame without consuming it.
    virtual uint32_t peekNextVideoRTPTimestamp() = 0;

protected:
    IMediaSource() = default;
};

} // namespace rtspserver::media
