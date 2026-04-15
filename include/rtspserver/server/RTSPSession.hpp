#pragma once

#include "rtspserver/media/IMediaSource.hpp"
#include "rtspserver/rtp/H264Packetizer.hpp"
#include "rtspserver/rtp/IRTPSender.hpp"
#include "rtspserver/rtp/RTPPacket.hpp"
#include "rtspserver/rtsp/RTSPRequest.hpp"
#include "rtspserver/rtsp/RTSPResponse.hpp"
#include "rtspserver/utils/NonBlockingQueue.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rtspserver::server {

/**
 * @brief Non blocking RTSP Session used by thye Reactor class.
 * Each client connection is represented by one RTSPSession instance,
 * which manages its own state, media source, and pacing.
 *
 *
 *
 * @tparam OutboxHWM  Bytes in the per-session write-outbox before a slow client is disconnected (default 5 MiB).
 * @tparam MaxRecvBuf Maximum bytes kept in the RTSP receive buffer before the connection is closed (default 16 KiB).
 * @tparam RecvChunk  Read() chunk size per Dispatcher wake (default 8 KiB).
 */
template <
    std::size_t OutboxHWM = 5 * 1024 * 1024, // 5 MiB
    std::size_t MaxRecvBuf = 16 * 1024, // 16 KiB
    std::size_t RecvChunk = 8192 // 8 KiB
    >
class RTSPSession {
public:
    RTSPSession(int fd, std::string client_ip,
        std::string media_root, int epoll_fd);
    ~RTSPSession();

    RTSPSession(const RTSPSession&) = delete;
    RTSPSession& operator=(const RTSPSession&) = delete;

    bool onReadable(std::vector<rtsp::RTSPRequest>& out);
    bool onWritable();
    void enableWrite();

    void pushCommand(rtsp::RTSPRequest req);

    void pacerTick(std::chrono::steady_clock::time_point now);

    /**
     * @brief Attempt to claim the pacer slot for this tick.
     *
     * @return true  The caller successfully acquired the slot.
     * @return false The slot is already in-flight.
     */
    bool tryClaimPacerTick() noexcept
    {
        bool expected = false;
        return pacer_in_flight_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel);
    }

    /**
     * @brief Release the pacer slot after completing the tick.
     * Should be called by the worker thread after finishing pacerTick().
     *
     */
    void releasePacerTick() noexcept
    {
        pacer_in_flight_.store(false, std::memory_order_release);
    }

    int fd() const { return fd_; }
    std::string sessionId() const { return session_id_; }

private:
    // RTSP Handlers
    void handleRequest(const rtsp::RTSPRequest& req);
    void handleOptions(const rtsp::RTSPRequest& req);
    void handleDescribe(const rtsp::RTSPRequest& req);
    void handleSetup(const rtsp::RTSPRequest& req);
    void handlePlay(const rtsp::RTSPRequest& req);
    void handlePause(const rtsp::RTSPRequest& req);
    void handleTeardown(const rtsp::RTSPRequest& req);

    // M<edia related helpers for pacers
    bool openMedia(const std::string& url, int cseq);
    void seekAndReset(double npt_secs, bool adjust_offset);

    // I/O helpers
    void sendResponse(const rtsp::RTSPResponse& resp);
    void sendVideoRTP(std::span<const uint8_t> payload, uint32_t ts, bool marker);
    void sendAudioRTP(std::span<const uint8_t> rtp_pkt);
    void pushTCPFrame(int channel, std::span<const uint8_t> rtp_pkt);

    enum class State { INIT,
        READY,
        PLAYING,
        PAUSED };
    using Clock = std::chrono::steady_clock;

    // socket details
    int fd_;
    int epoll_fd_;
    std::string client_ip_;
    std::string media_root_;

    State state_ { State::INIT };
    std::string session_id_;

    std::unique_ptr<media::IMediaSource> media_source_;
    rtp::H264Packetizer packetizer_;
    bool describe_done_ { false };

    std::unique_ptr<rtp::IRTPSender> rtp_sender_;
    std::unique_ptr<rtp::IRTPSender> audio_rtp_sender_;

    // use_tcp_ help to selection between udp and tcp based on the request
    bool use_tcp_ { false };
    int rtp_channel_ { 0 };
    int rtcp_channel_ { 1 };
    int audio_rtp_channel_ { 2 };
    int audio_rtcp_channel_ { 3 };
    int payload_type_ { 96 };
    int audio_payload_type_ { 97 };
    bool transport_ready_ { false };
    bool audio_transport_ready_ { false };

    // Guard for pacer tick in-flight status.  Only accessed by the Logic and Pacer threads.
    std::atomic<bool> pacer_in_flight_ { false };

    // Pacing details
    Clock::time_point wall_start_ {};
    Clock::time_point pause_start_ {};
    bool first_frame_ { true };
    uint32_t loop_video_offset_ { 0 };
    uint32_t audio_loop_offset_ { 0 };

    std::optional<media::VideoPacket> pending_vp_;
    std::optional<media::AudioPacket> pending_ap_;

    uint32_t last_video_rtp_ { 0 };
    std::optional<uint32_t> last_frame_dur_;

    bool resume_waiting_for_idr_ = false;

    double last_keyframe_npt_ { 0.0 };
    double last_npt_ { 0.0 };
    double npt_end_ { -1.0 };

    // Rtp Sequence details
    uint16_t rtp_seq_ { 0 };
    uint32_t ssrc_ { 0 };
    uint16_t audio_rtp_seq_ { 0 };
    uint32_t audio_ssrc_ { 0 };

    // Pacer/Logic push, Dispatcher drain
    utils::NonBlockingQueue<std::vector<uint8_t>> outbox_;
    std::atomic<size_t> outbox_bytes_ { 0 };
    static constexpr std::size_t kOutboxHWM = OutboxHWM;

    std::optional<std::vector<uint8_t>> wbuf_;
    size_t woff_ { 0 };

    std::atomic<bool> epollout_armed_ { false };
    std::atomic<bool> close_after_write_ { false };

    // Receive buffer for accumulating RTSP requests.  Accessed only by the Dispatcher thread.
    std::string recv_buf_;
    static constexpr std::size_t kMaxRecvBuf = MaxRecvBuf;
    static constexpr std::size_t kRecvChunk = RecvChunk;

    // Logic to parser command queue.
    utils::NonBlockingQueue<rtsp::RTSPRequest> cmd_queue_;
};

// RTSPSession<> == RTSPSession<5MiB, 16KiB, 8KiB> — the common default.
using DefaultRTSPSession = RTSPSession<>;

// Suppress implicit instantiation in every TU; the explicit instantiation lives
// in RTSPSession.cpp and covers the default parameter set.
extern template class RTSPSession<>;

} // namespace rtspserver::server
