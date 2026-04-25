#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/backoff_strategy.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/grpc/status.h"
#include "envoy/http/codec.h"

#include "source/common/common/logger.h"
#include "source/common/grpc/codec.h"
#include "source/common/http/codec_client.h"
#include "source/common/http/response_decoder_impl_base.h"

#include "absl/strings/string_view.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Orca {

/**
 * Factory used by OrcaOobSession to allocate a fresh codec client for each
 * connection attempt. The factory is host-agnostic from the session's
 * perspective: the caller is responsible for creating the underlying
 * connection.
 */
using OrcaOobCreateCodecClientCb = std::function<Http::CodecClientPtr()>;

/**
 * Invoked for every successfully decoded ORCA load report received on the
 * stream. The reference is only valid for the duration of the call.
 */
using OrcaOobReportCb = std::function<void(const xds::data::orca::v3::OrcaLoadReport&)>;

/**
 * Invoked exactly once when the session has reached a terminal state and
 * will perform no further work. The session must not be touched after this
 * fires; the caller is expected to deferred-delete it. The grpc_status and
 * message describe the reason.
 */
using OrcaOobTerminatedCb = std::function<void(Grpc::Status::GrpcStatus, absl::string_view)>;

/**
 * Lifecycle event signal so the owner can update host metadata (e.g. mark a
 * stream as live) without coupling the session to host plumbing.
 */
enum class OrcaOobLifecycleEvent {
  StreamOpen,
  StreamFailure,
};

using OrcaOobLifecycleCb = std::function<void(OrcaOobLifecycleEvent)>;

/**
 * Host-agnostic leaf that drives a single ORCA out-of-band gRPC stream
 * (xds.service.orca.v3.OpenRcaService.StreamCoreMetrics) over a raw
 * Http::CodecClient. The session owns its codec client across retries,
 * delivers parsed reports to the report callback, and signals lifecycle
 * transitions to the owner.
 *
 * Modeled on the gRPC health checker pattern: per-attempt codec client,
 * deferred-delete on teardown, manual gRPC framing via Grpc::Decoder. Like
 * Http::CodecClient, the session is itself deferred-deletable so the
 * manager can hand it to dispatcher.deferredDelete() during shutdown.
 *
 * Ownership/destruction contract: callers must release the session via
 * dispatcher.deferredDelete(). Direct destruction is only safe outside any
 * session-originated callback frame because the dtor synchronously closes
 * and destroys the underlying CodecClient.
 *
 * Retry/backoff semantics:
 *  - On a first successful report, the supplied BackOffStrategy is reset.
 *  - If a transient failure occurs after at least one report has been
 *    received in the lifetime of the session, the next attempt is
 *    immediate; subsequent failures consult the strategy.
 *  - Transient failures fire StreamFailure lifecycle events.
 *  - A gRPC UNIMPLEMENTED status is terminal: on_terminated_ fires and no
 *    further retries are scheduled.
 *
 * GOAWAY handling:
 *  - GOAWAY(NoError) starts a bounded drain timer; the session continues to
 *    accept frames until the timer expires, then reconnects.
 *  - Any other GOAWAY error code closes the connection and schedules a
 *    backoff retry.
 */
class OrcaOobSession : public Event::DeferredDeletable,
                       public Http::ResponseDecoderImplBase,
                       public Http::StreamCallbacks,
                       public Http::ConnectionCallbacks,
                       protected Logger::Loggable<Logger::Id::upstream> {
public:
  OrcaOobSession(OrcaOobCreateCodecClientCb create_codec_client, Event::Dispatcher& dispatcher,
                 std::chrono::milliseconds report_interval,
                 std::vector<std::string> request_cost_names, bool upstream_secure_transport,
                 OrcaOobReportCb on_report, OrcaOobTerminatedCb on_terminated,
                 OrcaOobLifecycleCb on_lifecycle_event, BackOffStrategyPtr backoff);
  // IMPORTANT: callers must release the session via dispatcher.deferredDelete().
  // Direct destruction is only safe outside any session-originated callback frame
  // because the dtor synchronously closes and destroys the underlying CodecClient.
  ~OrcaOobSession() override;

  /**
   * Begin the session: open a stream immediately.
   * Idempotent; subsequent calls are ignored once the session has started.
   */
  void start();

  /**
   * Stop the session and suppress any further user callbacks. Closes the
   * codec client via deferred delete and disables timers. Safe to call
   * multiple times. Does NOT fire on_terminated_.
   */
  void close();

  // Bounded drain deadline for GOAWAY(NoError) before reconnecting. Public
  // so tests can reference the same constant.
  static constexpr std::chrono::milliseconds kGoAwayDrainDeadline{5000};

  // Http::StreamDecoder
  void decodeData(Buffer::Instance& data, bool end_stream) override;
  void decodeMetadata(Http::MetadataMapPtr&&) override {}

  // Http::ResponseDecoder
  void decode1xxHeaders(Http::ResponseHeaderMapPtr&&) override {}
  void decodeHeaders(Http::ResponseHeaderMapPtr&& headers, bool end_stream) override;
  void decodeTrailers(Http::ResponseTrailerMapPtr&& trailers) override;
  void dumpState(std::ostream&, int) const override {}

  // Http::StreamCallbacks
  void onResetStream(Http::StreamResetReason reason,
                     absl::string_view transport_failure_reason) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

  // Http::ConnectionCallbacks
  void onGoAway(Http::GoAwayErrorCode error_code) override;

private:
  // Network-level connection callbacks bridge.
  class ConnectionCallbackImpl : public Network::ConnectionCallbacks {
  public:
    ConnectionCallbackImpl(OrcaOobSession& parent) : parent_(parent) {}
    void onEvent(Network::ConnectionEvent event) override { parent_.onConnectionEvent(event); }
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

  private:
    OrcaOobSession& parent_;
  };

  void onConnectionEvent(Network::ConnectionEvent event);

  // Open a new codec client and stream.
  void connectAndStream();
  // Schedule a retry using the backoff strategy. If immediate is true,
  // schedule the retry with zero delay (used for the one-shot immediate
  // retry after first successful report).
  void scheduleRetry(bool immediate);
  // Tear down the active stream/connection and notify the owner of a
  // transient failure. If terminal is true, instead invoke on_terminated_;
  // the session must NOT be touched by the caller after that.
  void handleTransientFailure(absl::string_view reason);
  void handleTerminal(Grpc::Status::GrpcStatus status, absl::string_view reason);
  // Called when a complete OrcaLoadReport has been parsed.
  void onReport(const xds::data::orca::v3::OrcaLoadReport& report);
  // Reset stream/decoder state without touching the codec client.
  void resetStreamState();
  // Tear down the codec client via deferredDelete. Safe to call when null.
  void tearDownCodecClient();
  // Process the gRPC status reported by trailers/headers.
  void onRpcComplete(Grpc::Status::GrpcStatus status, absl::string_view message, bool end_stream);

  // Construction-time state.
  OrcaOobCreateCodecClientCb create_codec_client_;
  Event::Dispatcher& dispatcher_;
  const std::chrono::milliseconds report_interval_;
  const std::vector<std::string> request_cost_names_;
  const bool upstream_secure_transport_;
  OrcaOobReportCb on_report_;
  OrcaOobTerminatedCb on_terminated_;
  OrcaOobLifecycleCb on_lifecycle_event_;
  BackOffStrategyPtr backoff_;

  ConnectionCallbackImpl connection_callback_impl_{*this};
  Event::TimerPtr retry_timer_;
  Event::TimerPtr goaway_drain_timer_;

  Http::CodecClientPtr codec_client_;
  Http::RequestEncoder* request_encoder_{nullptr};
  Grpc::Decoder decoder_;

  // True if start() has been called.
  bool started_{false};
  // True if close() has been called or we have terminated; suppresses all
  // further user callbacks.
  bool stopped_{false};
  // True if at least one report has been received over the lifetime of the
  // session. Drives the "next failure gets one immediate retry" rule.
  bool received_any_report_{false};
  // True once we have observed a report within the current connection
  // attempt and used it to re-arm the backoff strategy. This guards the
  // backoff_->reset() call so that successive reports in a single attempt
  // do not redundantly reset the backoff.
  bool backoff_reset_armed_in_attempt_{false};
  // True if the immediate-retry budget is currently available (i.e. a
  // successful report has been observed and we have not yet consumed the
  // immediate retry).
  bool immediate_retry_available_{false};
  // True if we received a NoError GOAWAY and are waiting out the drain
  // window before reconnecting.
  bool draining_no_error_goaway_{false};
  // True if a stream reset is expected by us (e.g. we initiated the close
  // after observing an error).
  bool expect_reset_{false};
  // True while we are inside handleTransientFailure() / tearing down the
  // codec client; suppresses re-entrant transient failure handling that
  // would otherwise fire when close() synchronously dispatches
  // onConnectionEvent(LocalClose).
  bool tearing_down_{false};
};

using OrcaOobSessionPtr = std::unique_ptr<OrcaOobSession>;

} // namespace Orca
} // namespace Envoy
