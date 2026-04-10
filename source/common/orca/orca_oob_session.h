#pragma once

#include <chrono>
#include <memory>

#include "envoy/buffer/buffer.h"
#include "envoy/common/random_generator.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/grpc/status.h"
#include "envoy/http/codec.h"
#include "envoy/network/connection.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/backoff_strategy.h"
#include "source/common/common/logger.h"
#include "source/common/grpc/codec.h"
#include "source/common/http/codec_client.h"
#include "source/common/http/response_decoder_impl_base.h"

namespace Envoy {
namespace Orca {

/**
 * All OOB stats. @see stats_macros.h
 */
#define ALL_ORCA_OOB_STATS(COUNTER, GAUGE)                                                         \
  COUNTER(streams_started)                                                                         \
  COUNTER(streams_closed)                                                                          \
  COUNTER(streams_failed)                                                                          \
  COUNTER(reports_received)                                                                        \
  COUNTER(reports_failed)                                                                          \
  COUNTER(backend_not_supported)                                                                   \
  COUNTER(non_h2_host_skipped)                                                                     \
  COUNTER(reconnect_attempts)                                                                      \
  GAUGE(active_streams, NeverImport)

/**
 * Struct definition for OOB stats. @see stats_macros.h
 */
struct OrcaOobStats {
  ALL_ORCA_OOB_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

/**
 * Callback interface for OOB report consumers.
 */
class OrcaOobCallbacks {
public:
  virtual ~OrcaOobCallbacks() = default;
  virtual void onOrcaOobReport(const xds::data::orca::v3::OrcaLoadReport& report) PURE;
  virtual void onOrcaOobStreamFailure(Grpc::Status::GrpcStatus status) PURE;
};

/**
 * Manages a single OOB gRPC stream to one upstream host.
 *
 * Opens an HTTP/2 connection via host->createConnection() and calls
 * xds.service.orca.v3.OpenRcaService/StreamCoreMetrics. The client sends one
 * OrcaLoadReportRequest with a report_interval, and the server streams back
 * periodic OrcaLoadReport messages.
 *
 * Uses inner classes for Network::ConnectionCallbacks and
 * Http::ConnectionCallbacks to avoid diamond-inheritance ambiguity on
 * watermark methods (same pattern as GrpcActiveHealthCheckSession).
 *
 * Threading: the session is bound to the single Event::Dispatcher passed to
 * the constructor (typically the cluster manager / main thread dispatcher).
 * All methods except the constructor are expected to run on that dispatcher.
 * OrcaOobCallbacks::onOrcaOobReport is invoked on the same dispatcher thread.
 *
 * Concurrent state: because OOB callbacks run on the main-thread dispatcher
 * but in-band ORCA delivery runs on worker threads, any state touched from
 * both paths must be thread-safe. Today OrcaLoadReportHandler is stateless
 * (only const members: metric_names_for_computing_utilization_,
 * error_utilization_penalty_, time_source_) and OrcaHostLbPolicyData uses
 * atomic weight fields. Do NOT add mutable state to either without adding
 * explicit synchronization.
 *
 * Stats: OrcaOobStats is cluster-scoped in this iteration -- there is no
 * per-host breakdown. Per-host identification is available in debug logs
 * via host_->address()->asString().
 */
class OrcaOobSession : public Http::ResponseDecoderImplBase,
                       public Http::StreamCallbacks,
                       public Event::DeferredDeletable,
                       Logger::Loggable<Logger::Id::upstream> {
public:
  OrcaOobSession(Upstream::HostSharedPtr host, Event::Dispatcher& dispatcher,
                 Random::RandomGenerator& random, std::chrono::milliseconds reporting_period,
                 OrcaOobCallbacks& callbacks, OrcaOobStats& stats);
  ~OrcaOobSession() override;

  virtual void start();
  virtual void stop();

protected:
  // Virtual for test override. Creates the HTTP/2 codec client wrapping the connection.
  virtual Http::CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data);

private:
  void createClient();
  void startStream();
  void scheduleReconnect();
  enum class StreamEndReason { Closed, Failed, Unsupported };
  void cleanupStream(StreamEndReason reason);
  void resetStream();
  void handlePermanentFailure(Grpc::Status::WellKnownGrpcStatus status);

  // Http::ResponseDecoderImplBase
  void decode1xxHeaders(Http::ResponseHeaderMapPtr&&) override {}
  void decodeHeaders(Http::ResponseHeaderMapPtr&& headers, bool end_stream) override;
  void decodeData(Buffer::Instance& data, bool end_stream) override;
  void decodeTrailers(Http::ResponseTrailerMapPtr&& trailers) override;
  void decodeMetadata(Http::MetadataMapPtr&&) override {}
  void dumpState(std::ostream&, int) const override {}

  // Http::StreamCallbacks
  void onResetStream(Http::StreamResetReason reason, absl::string_view) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

  // Delegated handlers called from inner callback classes.
  void onEvent(Network::ConnectionEvent event);
  void onGoAway(Http::GoAwayErrorCode error_code);

  // Inner class for Network::ConnectionCallbacks (avoids diamond ambiguity).
  class ConnectionCallbackImpl : public Network::ConnectionCallbacks {
  public:
    ConnectionCallbackImpl(OrcaOobSession& parent) : parent_(parent) {}
    void onEvent(Network::ConnectionEvent event) override { parent_.onEvent(event); }
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

  private:
    OrcaOobSession& parent_;
  };

  // Inner class for Http::ConnectionCallbacks (avoids diamond ambiguity).
  class HttpConnectionCallbackImpl : public Http::ConnectionCallbacks {
  public:
    HttpConnectionCallbackImpl(OrcaOobSession& parent) : parent_(parent) {}
    void onGoAway(Http::GoAwayErrorCode error_code) override { parent_.onGoAway(error_code); }

  private:
    OrcaOobSession& parent_;
  };

  void onOrcaLoadReportReceived(Buffer::InstancePtr&& message);

  Upstream::HostSharedPtr host_;
  Event::Dispatcher& dispatcher_;
  std::chrono::milliseconds reporting_period_;
  OrcaOobCallbacks& callbacks_;
  OrcaOobStats& stats_;

  Http::CodecClientPtr client_;
  Http::RequestEncoder* request_encoder_{};
  Grpc::Decoder grpc_decoder_;
  bool stream_closed_{true};
  bool backoff_reset_for_active_stream_{false};

  ConnectionCallbackImpl connection_cb_{*this};
  HttpConnectionCallbackImpl http_connection_cb_{*this};

  Event::TimerPtr reconnect_timer_;
  BackOffStrategyPtr backoff_strategy_;

  // If the server returns a permanent gRPC failure (UNIMPLEMENTED,
  // UNAUTHENTICATED, PERMISSION_DENIED, INVALID_ARGUMENT), stop retrying.
  bool server_supports_oob_{true};

  // Set during stop() to suppress reconnect attempts from re-entrant callbacks.
  bool stopping_{false};

  // Set during stale-client teardown in start() so re-entrant reconnect
  // scheduling from the old client's close callback is a no-op.
  bool replacing_client_{false};

  Random::RandomGenerator& random_;
};

using OrcaOobSessionPtr = std::unique_ptr<OrcaOobSession>;

} // namespace Orca
} // namespace Envoy
