#include "source/common/orca/orca_oob_session.h"

#include <chrono>
#include <memory>

#include "source/common/buffer/zero_copy_input_stream_impl.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codec_client.h"
#include "source/common/http/utility.h"

#include "xds/data/orca/v3/orca_load_report.pb.h"
#include "xds/service/orca/v3/orca.pb.h"

namespace Envoy {
namespace Orca {

OrcaOobSession::OrcaOobSession(Upstream::HostSharedPtr host, Event::Dispatcher& dispatcher,
                               Random::RandomGenerator& random,
                               std::chrono::milliseconds reporting_period,
                               OrcaOobCallbacks& callbacks, OrcaOobStats& stats)
    : host_(std::move(host)), dispatcher_(dispatcher), reporting_period_(reporting_period),
      callbacks_(callbacks), stats_(stats), random_(random) {
  const uint64_t max_interval_ms =
      std::min(static_cast<uint64_t>(reporting_period_.count() * 6), static_cast<uint64_t>(60000));
  backoff_strategy_ = std::make_unique<JitteredExponentialBackOffStrategy>(
      1000, std::max(max_interval_ms, static_cast<uint64_t>(1000)), random_);
  reconnect_timer_ = dispatcher_.createTimer([this]() {
    createClient();
    startStream();
  });
}

OrcaOobSession::~OrcaOobSession() { stop(); }

void OrcaOobSession::start() {
  if (!server_supports_oob_) {
    return;
  }
  createClient();
  startStream();
}

void OrcaOobSession::stop() {
  if (request_encoder_ && !stream_closed_) {
    request_encoder_->getStream().resetStream(Http::StreamResetReason::LocalReset);
  }
  request_encoder_ = nullptr;
  stream_closed_ = true;
  if (client_) {
    client_->close(Network::ConnectionCloseType::NoFlush);
    client_.reset();
  }
  // Disable reconnect timer AFTER close() because close() synchronously fires
  // onEvent(LocalClose) which calls scheduleReconnect(). Disabling after
  // ensures the timer stays disarmed.
  if (reconnect_timer_) {
    reconnect_timer_->disableTimer();
  }
}

Http::CodecClientPtr
OrcaOobSession::createCodecClient(Upstream::Host::CreateConnectionData& data) {
  return std::make_unique<Http::CodecClientProd>(
      Http::CodecType::HTTP2, std::move(data.connection_), data.host_description_, dispatcher_,
      random_, /*transport_socket_options=*/nullptr);
}

void OrcaOobSession::createClient() {
  auto conn_data = host_->createConnection(dispatcher_, /*options=*/nullptr,
                                           /*transport_socket_options=*/nullptr);
  client_ = createCodecClient(conn_data);
  client_->addConnectionCallbacks(connection_cb_);
  client_->setCodecConnectionCallbacks(http_connection_cb_);
}

void OrcaOobSession::startStream() {
  if (!client_) {
    return;
  }

  request_encoder_ = &client_->newStream(*this);
  request_encoder_->getStream().addCallbacks(*this);

  // Build :authority header with fallback for IP-only endpoints.
  const std::string authority = host_->hostname().empty() ? host_->address()->asString()
                                                         : std::string(host_->hostname());
  auto headers =
      Grpc::Common::prepareHeaders(authority, "xds.service.orca.v3.OpenRcaService",
                                   "StreamCoreMetrics", /*timeout=*/absl::nullopt);

  auto status = request_encoder_->encodeHeaders(headers->headers(), false);
  ASSERT(status.ok());

  // Send OrcaLoadReportRequest with report_interval.
  xds::service::orca::v3::OrcaLoadReportRequest request;
  request.mutable_report_interval()->set_seconds(reporting_period_.count() / 1000);
  request.mutable_report_interval()->set_nanos((reporting_period_.count() % 1000) * 1000000);

  // end_stream=true: client sends one message, server streams responses.
  request_encoder_->encodeData(*Grpc::Common::serializeToGrpcFrame(request), true);

  stream_closed_ = false;
  backoff_strategy_->reset();
  stats_.streams_started_.inc();
  stats_.active_streams_.inc();
}

void OrcaOobSession::decodeHeaders(Http::ResponseHeaderMapPtr&& headers, bool end_stream) {
  const auto http_response_status = Http::Utility::getResponseStatus(*headers);
  if (http_response_status != 200) {
    if (end_stream) {
      const auto grpc_status = Grpc::Common::getGrpcStatus(*headers);
      if (grpc_status &&
          grpc_status.value() == Grpc::Status::WellKnownGrpcStatus::Unimplemented) {
        server_supports_oob_ = false;
        stats_.backend_not_supported_.inc();
        stats_.active_streams_.dec();
        stream_closed_ = true;
        callbacks_.onOrcaOobStreamFailure(grpc_status.value());
        return;
      }
    }
    onStreamComplete();
    return;
  }
  if (end_stream) {
    const auto grpc_status = Grpc::Common::getGrpcStatus(*headers);
    if (grpc_status &&
        grpc_status.value() == Grpc::Status::WellKnownGrpcStatus::Unimplemented) {
      server_supports_oob_ = false;
      stats_.backend_not_supported_.inc();
      stats_.active_streams_.dec();
      stream_closed_ = true;
      callbacks_.onOrcaOobStreamFailure(grpc_status.value());
      return;
    }
    onStreamComplete();
  }
}

void OrcaOobSession::decodeData(Buffer::Instance& data, bool end_stream) {
  std::vector<Grpc::Frame> frames;
  const auto status = grpc_decoder_.decode(data, frames);
  if (!status.ok()) {
    ENVOY_LOG(debug, "OrcaOobSession: gRPC decode error for host {}: {}",
              host_->address()->asString(), status.message());
    stats_.streams_failed_.inc();
    resetStream();
    return;
  }
  for (auto& frame : frames) {
    onOrcaLoadReportReceived(std::move(frame.data_));
  }
  if (end_stream) {
    onStreamComplete();
  }
}

void OrcaOobSession::decodeTrailers(Http::ResponseTrailerMapPtr&& trailers) {
  auto maybe_grpc_status = Grpc::Common::getGrpcStatus(*trailers);
  if (maybe_grpc_status.has_value()) {
    if (maybe_grpc_status.value() == Grpc::Status::WellKnownGrpcStatus::Unimplemented) {
      server_supports_oob_ = false;
      stats_.backend_not_supported_.inc();
      if (!stream_closed_) {
        stats_.active_streams_.dec();
      }
      stream_closed_ = true;
      request_encoder_ = nullptr;
      grpc_decoder_ = Grpc::Decoder();
      callbacks_.onOrcaOobStreamFailure(maybe_grpc_status.value());
      return;
    }
  }
  onStreamComplete();
}

void OrcaOobSession::onOrcaLoadReportReceived(Buffer::InstancePtr&& message) {
  xds::data::orca::v3::OrcaLoadReport report;
  Buffer::ZeroCopyInputStreamImpl stream(std::move(message));
  if (!report.ParseFromZeroCopyStream(&stream)) {
    ENVOY_LOG(debug, "OrcaOobSession: failed to parse OrcaLoadReport from host {}",
              host_->address()->asString());
    stats_.reports_failed_.inc();
    return;
  }
  stats_.reports_received_.inc();
  callbacks_.onOrcaOobReport(report);
}

void OrcaOobSession::onResetStream(Http::StreamResetReason, absl::string_view) {
  if (!stream_closed_) {
    stats_.active_streams_.dec();
    stats_.streams_failed_.inc();
  }
  stream_closed_ = true;
  request_encoder_ = nullptr;
  grpc_decoder_ = Grpc::Decoder();
  scheduleReconnect();
}

void OrcaOobSession::onEvent(Network::ConnectionEvent event) {
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    if (!stream_closed_) {
      stats_.active_streams_.dec();
      stats_.streams_failed_.inc();
    }
    stream_closed_ = true;
    request_encoder_ = nullptr;
    grpc_decoder_ = Grpc::Decoder();
    // Do NOT call client_.reset() here. This callback may fire synchronously
    // from within client_->close(), so destroying the client here would be
    // use-after-free. The client is cleaned up by stop() or replaced by
    // createClient() on reconnect.
    scheduleReconnect();
  }
}

void OrcaOobSession::onGoAway(Http::GoAwayErrorCode) {
  // Close the connection directly. The onEvent(LocalClose) callback will
  // handle stream state cleanup and schedule reconnect. We intentionally
  // do NOT reset the stream separately here to avoid double-backoff:
  // resetStream() would trigger onResetStream() → scheduleReconnect(),
  // then close() would trigger onEvent() → scheduleReconnect() again,
  // advancing the backoff by two steps for a single GOAWAY event.
  if (client_) {
    client_->close(Network::ConnectionCloseType::NoFlush);
  }
}

void OrcaOobSession::onStreamComplete() {
  if (!stream_closed_) {
    stats_.active_streams_.dec();
    stats_.streams_closed_.inc();
  }
  stream_closed_ = true;
  request_encoder_ = nullptr;
  grpc_decoder_ = Grpc::Decoder();
  scheduleReconnect();
}

void OrcaOobSession::resetStream() {
  if (request_encoder_ && !stream_closed_) {
    request_encoder_->getStream().resetStream(Http::StreamResetReason::LocalReset);
  }
  // onResetStream callback will handle cleanup and reconnect.
}

void OrcaOobSession::scheduleReconnect() {
  if (!server_supports_oob_) {
    return;
  }
  // Guard against double-scheduling within a single event chain (e.g., GOAWAY
  // where the connection close triggers both onResetStream and onEvent, or
  // decodeHeaders followed by onEvent). Without this guard, the backoff would
  // advance by two steps for a single failure event.
  if (reconnect_timer_->enabled()) {
    return;
  }
  const uint64_t backoff_ms = backoff_strategy_->nextBackOffMs();
  ENVOY_LOG(debug, "OrcaOobSession: scheduling reconnect to {} in {}ms",
            host_->address()->asString(), backoff_ms);
  reconnect_timer_->enableTimer(std::chrono::milliseconds(backoff_ms));
}

} // namespace Orca
} // namespace Envoy
