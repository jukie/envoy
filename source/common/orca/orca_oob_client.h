#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/backoff_strategy.h"
#include "envoy/common/random_generator.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/grpc/async_client.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"

#include "source/common/common/logger.h"
#include "source/common/grpc/typed_async_client.h"

#include "absl/strings/string_view.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"
#include "xds/service/orca/v3/orca.pb.h"

namespace Envoy {
namespace Orca {

/**
 * All stats for the ORCA OOB client. @see stats_macros.h
 *
 * - stream_success: first report received on a stream (backoff reset event).
 * - stream_failure: each transient stream close.
 * - stream_terminated: exactly once per client when it permanently stops
 *   (UNIMPLEMENTED, caller close(), or destruction - idempotent).
 * - reports_received: total decoded OrcaLoadReport messages.
 * - stream_active (gauge): 1 while the current stream has received at least
 *   one report and is still open, 0 otherwise.
 */
#define ALL_ORCA_OOB_CLIENT_STATS(COUNTER, GAUGE)                                                  \
  COUNTER(stream_success)                                                                          \
  COUNTER(stream_failure)                                                                          \
  COUNTER(stream_terminated)                                                                       \
  COUNTER(reports_received)                                                                        \
  GAUGE(stream_active, NeverImport)

/**
 * Struct definition for ORCA OOB client stats. @see stats_macros.h
 */
struct OrcaOobClientStats {
  ALL_ORCA_OOB_CLIENT_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

/**
 * Construct an OrcaOobClientStats rooted in the given scope with the given
 * prefix. Prefix should end in '.'.
 */
OrcaOobClientStats generateOrcaOobClientStats(Stats::Scope& scope, absl::string_view prefix);

/**
 * Subscriber interface for a single OrcaOobClient. All methods are invoked
 * on the client's dispatcher thread.
 */
class OrcaOobReportCallbacks {
public:
  virtual ~OrcaOobReportCallbacks() = default;

  /**
   * Called on every decoded OrcaLoadReport. Invoked on the client's
   * dispatcher thread.
   */
  virtual void onOrcaReport(const xds::data::orca::v3::OrcaLoadReport& report) PURE;

  /**
   * Called once, when a backend- or protocol-initiated terminal event
   * permanently stops the client. Primary trigger is UNIMPLEMENTED from the
   * backend. NOT called when the caller invokes
   * close() or destroys the client - the caller already knows in those cases
   * and a callback would create reentrant hazards.
   */
  virtual void onStreamClosed(Grpc::Status::GrpcStatus status, absl::string_view message) PURE;
};

/**
 * Single-subscriber client for the ORCA out-of-band metrics reporting
 * protocol. Opens one xds.service.orca.v3.OpenRcaService.StreamCoreMetrics
 * server-streaming RPC against a specific upstream host (pinned internally
 * via setUpstreamOverrideHost with strict=true on Http::AsyncClient::StreamOptions)
 * and delivers decoded reports to a subscriber.
 *
 * This class is thread-affine to the dispatcher passed at construction. All
 * public methods including close() and the destructor MUST be invoked on
 * the dispatcher thread. The class is not thread-safe.
 */
class OrcaOobClient : public Grpc::AsyncStreamCallbacks<xds::data::orca::v3::OrcaLoadReport>,
                      protected Logger::Loggable<Logger::Id::upstream> {
public:
  /**
   * Construct and eagerly open the stream. On construction the client:
   *   - Wraps the raw async client in a typed Grpc::AsyncClient.
   *   - Takes ownership of request_cost_names and target_address.
   *   - Calls backoff->reset() to normalize initial state.
   *   - Creates the retry timer.
   *   - Invokes openStream() immediately.
   *
   * The caller is responsible for selecting a valid target_address; the
   * library does not interpret it beyond passing it through as a strict
   * upstream override host.
   *
   * @param async_client         gRPC transport (obtained from AsyncClientManager).
   * @param dispatcher           owns all callbacks and timers; must outlive the client.
   * @param report_interval      desired report cadence; server clamps to its
   *                             configured minimum (gRFC A51 default 30s).
   * @param request_cost_names   passed through to the server; moved into
   *                             owned storage.
   * @param target_address       address of the upstream host to stream
   *                             reports from; moved into owned storage.
   * @param stats                stats struct owned by caller; must outlive client.
   * @param callbacks            subscriber; must outlive client.
   * @param backoff              required (non-null). Use defaultBackoffStrategy()
   *                             for the library's recommended defaults.
   */
  OrcaOobClient(Grpc::RawAsyncClientSharedPtr async_client, Event::Dispatcher& dispatcher,
                std::chrono::milliseconds report_interval,
                std::vector<std::string> request_cost_names, std::string target_address,
                OrcaOobClientStats& stats, OrcaOobReportCallbacks& callbacks,
                BackOffStrategyPtr backoff);

  ~OrcaOobClient() override;

  /**
   * Construct the library's recommended default backoff strategy:
   * JitteredExponentialBackOffStrategy with
   * Envoy::Config::SubscriptionFactory::RetryInitialDelayMs base and
   * Envoy::Config::SubscriptionFactory::RetryMaxDelayMs max.
   */
  static BackOffStrategyPtr defaultBackoffStrategy(Random::RandomGenerator& random);

  /**
   * Permanently stops the client. Idempotent. Does NOT fire any subscriber
   * callback - the caller already knows the client is stopping. Must be
   * called on the dispatcher thread.
   */
  void close();

  // Grpc::AsyncStreamCallbacks
  void onCreateInitialMetadata(Http::RequestHeaderMap& metadata) override;
  void onReceiveInitialMetadata(Http::ResponseHeaderMapPtr&& metadata) override;
  void onReceiveMessage(std::unique_ptr<xds::data::orca::v3::OrcaLoadReport>&& message) override;
  void onReceiveTrailingMetadata(Http::ResponseTrailerMapPtr&& metadata) override;
  void onRemoteClose(Grpc::Status::GrpcStatus status, const std::string& message) override;

private:
  void openStream();
  void scheduleRetry();
  xds::service::orca::v3::OrcaLoadReportRequest buildRequest() const;

  const Protobuf::MethodDescriptor& service_method_;
  Grpc::AsyncClient<xds::service::orca::v3::OrcaLoadReportRequest,
                    xds::data::orca::v3::OrcaLoadReport>
      async_client_;
  Event::Dispatcher& dispatcher_;
  const std::chrono::milliseconds report_interval_;
  const std::vector<std::string> request_cost_names_;
  const std::string target_address_;
  OrcaOobClientStats& stats_;
  OrcaOobReportCallbacks& callbacks_;
  BackOffStrategyPtr backoff_;

  Event::TimerPtr retry_timer_;
  Grpc::AsyncStream<xds::service::orca::v3::OrcaLoadReportRequest> stream_{};
  bool first_message_received_{false};
  bool closed_{false};
};

} // namespace Orca
} // namespace Envoy
