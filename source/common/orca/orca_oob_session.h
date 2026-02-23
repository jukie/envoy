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
 * Uses separate inner classes for Network::ConnectionCallbacks and
 * Http::ConnectionCallbacks to avoid diamond-inheritance ambiguity
 * on onAboveWriteBufferHighWatermark/onBelowWriteBufferLowWatermark
 * (same pattern as GrpcActiveHealthCheckSession).
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

  // Start the OOB stream (creates connection if needed).
  void start();

  // Stop and clean up. Safe to call during destruction.
  void stop();

protected:
  // Virtual for test override. Creates the HTTP/2 codec client wrapping the connection.
  // The default implementation creates a CodecClientProd with HTTP/2.
  //
  // Note: transport_socket_options is nullptr because OOB connections have no downstream
  // context to derive options from. The host's cluster transport socket factory already
  // provides TLS configuration. This matches the pattern used by ClusterManager::tcpConn()
  // when there is no LoadBalancerContext.
  virtual Http::CodecClientPtr
  createCodecClient(Upstream::Host::CreateConnectionData& data);

private:
  // Create the HTTP/2 connection to the host.
  void createClient();

  // Open the StreamCoreMetrics gRPC stream on the connection.
  void startStream();

  // Schedule a reconnect after failure.
  void scheduleReconnect();

  // Handle stream completion (server ended stream).
  void onStreamComplete();

  // Reset the active stream.
  void resetStream();

  // --- Http::ResponseDecoderImplBase ---
  void decode1xxHeaders(Http::ResponseHeaderMapPtr&&) override {}
  void decodeHeaders(Http::ResponseHeaderMapPtr&& headers, bool end_stream) override;
  void decodeData(Buffer::Instance& data, bool end_stream) override;
  void decodeTrailers(Http::ResponseTrailerMapPtr&& trailers) override;
  void decodeMetadata(Http::MetadataMapPtr&&) override {}
  void dumpState(std::ostream&, int) const override {}

  // --- Http::StreamCallbacks ---
  void onResetStream(Http::StreamResetReason reason, absl::string_view) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

  // Delegated handlers (called from inner callback classes).
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

  // Process a fully-framed gRPC message.
  void onOrcaLoadReportReceived(Buffer::InstancePtr&& message);

  Upstream::HostSharedPtr host_;
  Event::Dispatcher& dispatcher_;
  std::chrono::milliseconds reporting_period_;
  OrcaOobCallbacks& callbacks_;
  OrcaOobStats& stats_;

  Http::CodecClientPtr client_;             // HTTP/2 connection
  Http::RequestEncoder* request_encoder_{}; // Active gRPC stream
  Grpc::Decoder grpc_decoder_;              // gRPC frame decoder
  bool stream_closed_{true};

  ConnectionCallbackImpl connection_cb_{*this};
  HttpConnectionCallbackImpl http_connection_cb_{*this};

  Event::TimerPtr reconnect_timer_;
  // Backoff: 1s base, capped at min(oob_reporting_period * 6, 60s).
  // Reset on successful stream establishment.
  BackOffStrategyPtr backoff_strategy_;

  // If server returns UNIMPLEMENTED, stop retrying.
  bool server_supports_oob_{true};

  Random::RandomGenerator& random_;
};

} // namespace Orca
} // namespace Envoy
