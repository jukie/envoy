#include "source/common/orca/orca_oob_client.h"

#include "envoy/config/subscription_factory.h"

#include "source/common/common/assert.h"
#include "source/common/common/backoff_strategy.h"
#include "source/common/grpc/common.h"
#include "source/common/protobuf/protobuf.h"

namespace Envoy {
namespace Orca {

OrcaOobClientStats generateOrcaOobClientStats(Stats::Scope& scope, absl::string_view prefix) {
  const std::string prefix_str(prefix);
  return OrcaOobClientStats{ALL_ORCA_OOB_CLIENT_STATS(POOL_COUNTER_PREFIX(scope, prefix_str),
                                                      POOL_GAUGE_PREFIX(scope, prefix_str))};
}

BackOffStrategyPtr OrcaOobClient::defaultBackoffStrategy(Random::RandomGenerator& random) {
  return std::make_unique<JitteredExponentialBackOffStrategy>(
      Envoy::Config::SubscriptionFactory::RetryInitialDelayMs,
      Envoy::Config::SubscriptionFactory::RetryMaxDelayMs, random);
}

OrcaOobClient::OrcaOobClient(Grpc::RawAsyncClientSharedPtr async_client,
                             Event::Dispatcher& dispatcher,
                             std::chrono::milliseconds report_interval,
                             std::vector<std::string> request_cost_names,
                             Upstream::LoadBalancerContext::OverrideHost override_host,
                             OrcaOobClientStats& stats, OrcaOobReportCallbacks& callbacks,
                             BackOffStrategyPtr backoff)
    : service_method_(*Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
          "xds.service.orca.v3.OpenRcaService.StreamCoreMetrics")),
      async_client_(std::move(async_client)), dispatcher_(dispatcher),
      report_interval_(report_interval), request_cost_names_(std::move(request_cost_names)),
      override_host_address_(override_host.host), stats_(stats), callbacks_(callbacks),
      backoff_(std::move(backoff)) {
  ASSERT(backoff_ != nullptr);
  RELEASE_ASSERT(override_host.strict,
                 "OrcaOobClient requires strict host pinning; override_host.strict must be true");
  backoff_->reset();
  retry_timer_ = dispatcher_.createTimer([this] { openStream(); });
  openStream();
}

OrcaOobClient::~OrcaOobClient() {
  if (!closed_) {
    close();
  }
}

void OrcaOobClient::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  ENVOY_LOG(debug, "ORCA OOB: stream to {} closed by caller", override_host_address_);
  retry_timer_->disableTimer();
  if (stream_ != nullptr) {
    stream_.resetStream();
    stream_ = {};
  }
  stats_.stream_terminated_.inc();
  stats_.stream_active_.set(0);
}

void OrcaOobClient::openStream() {
  if (closed_) {
    return;
  }

  ENVOY_LOG(debug, "ORCA OOB: opening stream to {}", override_host_address_);

  Http::AsyncClient::StreamOptions options;
  options.setUpstreamOverrideHost(
      Upstream::LoadBalancerContext::OverrideHost{override_host_address_, /*strict=*/true});

  stream_ = async_client_.start(service_method_, *this, options);
  if (stream_ == nullptr) {
    ENVOY_LOG(debug, "ORCA OOB: start() returned null; scheduling retry");
    scheduleRetry();
    return;
  }

  stream_.sendMessage(buildRequest(), /*end_stream=*/true);
}

void OrcaOobClient::scheduleRetry() {
  if (closed_) {
    return;
  }
  std::chrono::milliseconds delay;
  if (first_message_received_) {
    // gRFC A51: "the next attempt will occur immediately" - the first retry after
    // a successful-then-failed stream fires at 0ms. The latch is one-shot;
    // subsequent failures without another successful message ramp from the
    // base delay again (backoff was reset when the first message arrived).
    delay = std::chrono::milliseconds(0);
    first_message_received_ = false;
  } else {
    delay = std::chrono::milliseconds(backoff_->nextBackOffMs());
  }
  retry_timer_->enableTimer(delay);
}

xds::service::orca::v3::OrcaLoadReportRequest OrcaOobClient::buildRequest() const {
  xds::service::orca::v3::OrcaLoadReportRequest request;
  const int64_t total_ns = std::chrono::nanoseconds(report_interval_).count();
  request.mutable_report_interval()->set_seconds(total_ns / 1'000'000'000);
  request.mutable_report_interval()->set_nanos(static_cast<int32_t>(total_ns % 1'000'000'000));
  for (const auto& name : request_cost_names_) {
    request.add_request_cost_names(name);
  }
  return request;
}

void OrcaOobClient::onCreateInitialMetadata(Http::RequestHeaderMap&) {}

void OrcaOobClient::onReceiveInitialMetadata(Http::ResponseHeaderMapPtr&&) {}

void OrcaOobClient::onReceiveMessage(
    std::unique_ptr<xds::data::orca::v3::OrcaLoadReport>&& message) {
  stats_.reports_received_.inc();
  if (!first_message_received_) {
    first_message_received_ = true;
    backoff_->reset();
    stats_.stream_success_.inc();
    stats_.stream_active_.set(1);
    ENVOY_LOG(debug, "ORCA OOB: first report received from {}; resetting backoff",
              override_host_address_);
  }
  callbacks_.onOrcaReport(*message);
}

void OrcaOobClient::onReceiveTrailingMetadata(Http::ResponseTrailerMapPtr&&) {}

void OrcaOobClient::onRemoteClose(Grpc::Status::GrpcStatus status, const std::string& message) {
  stream_ = {};
  stats_.stream_active_.set(0);

  if (closed_) {
    return;
  }

  if (status == Grpc::Status::WellKnownGrpcStatus::Unimplemented) {
    ENVOY_LOG(error,
              "ORCA OOB: backend at {} does not implement OpenRcaService; disabling OOB for "
              "this client",
              override_host_address_);
    stats_.stream_terminated_.inc();
    closed_ = true;
    callbacks_.onStreamClosed(status, message);
    return;
  }

  stats_.stream_failure_.inc();
  ENVOY_LOG(debug, "ORCA OOB: stream to {} closed transiently: {} ({})", override_host_address_,
            static_cast<int>(status), message);
  scheduleRetry();
}

} // namespace Orca
} // namespace Envoy
