#include "source/common/orca/orca_oob_session.h"

#include <chrono>
#include <memory>

#include "source/common/buffer/zero_copy_input_stream_impl.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codec_client.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h"

#include "xds/data/orca/v3/orca_load_report.pb.h"
#include "xds/service/orca/v3/orca.pb.h"

namespace Envoy {
namespace Orca {

namespace {
// Backoff for OOB gRPC stream reconnect. Independent of reporting period:
// the reconnect loop is driven by failure cadence, not success cadence.
constexpr uint64_t InitialBackoffMs = 1000; // 1s
constexpr uint64_t MaxBackoffMs = 60000;    // 60s
} // namespace

OrcaOobSession::OrcaOobSession(Upstream::HostSharedPtr host, Event::Dispatcher& dispatcher,
                               Random::RandomGenerator& random,
                               std::chrono::milliseconds reporting_period,
                               OrcaOobCallbacks& callbacks, OrcaOobStats& stats)
    : host_(std::move(host)), dispatcher_(dispatcher), reporting_period_(reporting_period),
      callbacks_(callbacks), stats_(stats), random_(random) {
  backoff_strategy_ =
      std::make_unique<JitteredExponentialBackOffStrategy>(InitialBackoffMs, MaxBackoffMs, random_);
  reconnect_timer_ = dispatcher_.createTimer([this]() { start(); });
}

OrcaOobSession::~OrcaOobSession() { stop(); }

void OrcaOobSession::start() {
  if (!server_supports_oob_) {
    return;
  }
  // start() rearms the session: a previous stop() -- including the one the
  // destructor runs -- may have left stopping_ set. Clear it so the next
  // failure's scheduleReconnect() is not unconditionally suppressed.
  stopping_ = false;
  // Close any stale client before creating a new one. Mirror stop()'s stat
  // bookkeeping and set replacing_client_ so the re-entrant cleanupStream() /
  // scheduleReconnect() path from the close callback is a no-op. The hard
  // close tears down the underlying stream implicitly.
  if (client_) {
    if (!stream_closed_) {
      stream_closed_ = true;
      stats_.active_streams_.dec();
    }
    request_encoder_ = nullptr;
    replacing_client_ = true;
    client_->close(Network::ConnectionCloseType::NoFlush);
    client_.reset();
    replacing_client_ = false;
  }
  createClient();
  startStream();
}

void OrcaOobSession::stop() {
  // Suppress reconnect attempts from re-entrant callbacks during teardown.
  stopping_ = true;
  // Mark stream closed before resetStream to avoid counting intentional stops
  // as failures in onResetStream.
  if (!stream_closed_) {
    stream_closed_ = true;
    stats_.active_streams_.dec();
  }
  if (request_encoder_) {
    request_encoder_->getStream().resetStream(Http::StreamResetReason::LocalReset);
  }
  request_encoder_ = nullptr;
  if (client_) {
    client_->close(Network::ConnectionCloseType::NoFlush);
    client_.reset();
  }
  if (reconnect_timer_) {
    reconnect_timer_->disableTimer();
  }
}

Http::CodecClientPtr OrcaOobSession::createCodecClient(Upstream::Host::CreateConnectionData& data) {
  // TODO(jukie): plumb transport_socket_options through the session so TLS
  // upstreams get SNI/ALPN configured correctly. Out of scope for MVP.
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
  // Ensure the decoder is fresh even if a prior reconnect path skipped
  // cleanupStream(). Invariant: every new stream starts with a pristine decoder.
  grpc_decoder_ = Grpc::Decoder();

  if (!client_) {
    return;
  }

  request_encoder_ = &client_->newStream(*this);
  request_encoder_->getStream().addCallbacks(*this);

  const std::string authority =
      host_->hostname().empty() ? host_->address()->asString() : std::string(host_->hostname());
  auto headers = Grpc::Common::prepareHeaders(authority, "xds.service.orca.v3.OpenRcaService",
                                              "StreamCoreMetrics", /*timeout=*/absl::nullopt);

  auto status = request_encoder_->encodeHeaders(headers->headers(), false);
  if (!status.ok()) {
    ENVOY_LOG(debug, "OrcaOobSession: encodeHeaders failed for host {}: {}",
              host_->address()->asString(), status.message());
    request_encoder_->getStream().resetStream(Http::StreamResetReason::LocalReset);
    request_encoder_ = nullptr;
    if (client_) {
      client_->close(Network::ConnectionCloseType::NoFlush);
      client_.reset();
    }
    stats_.streams_failed_.inc();
    scheduleReconnect();
    return;
  }

  xds::service::orca::v3::OrcaLoadReportRequest request;
  *request.mutable_report_interval() =
      Protobuf::util::TimeUtil::MillisecondsToDuration(reporting_period_.count());

  request_encoder_->encodeData(*Grpc::Common::serializeToGrpcFrame(request), true);

  stream_closed_ = false;
  backoff_reset_for_active_stream_ = false;
  // Clear the timer's enabled() state so a subsequent scheduleReconnect() isn't
  // suppressed by the stale flag when start() runs outside a timer callback.
  reconnect_timer_->disableTimer();
  stats_.streams_started_.inc();
  stats_.active_streams_.inc();
}

void OrcaOobSession::decodeData(Buffer::Instance& data, bool end_stream) {
  // Once the session has marked the stream closed (decode error, permanent
  // failure, reset, teardown), any further decode callbacks from the codec
  // must be ignored. The underlying HTTP/2 stream is torn down implicitly
  // when the client is destroyed on the next start() or deferredDelete, but
  // until then stray frames could otherwise be delivered to the callback and
  // reset the reconnect backoff -- see onOrcaLoadReportReceived.
  if (stream_closed_) {
    return;
  }
  std::vector<Grpc::Frame> frames;
  const auto status = grpc_decoder_.decode(data, frames);
  if (!status.ok()) {
    ENVOY_LOG(debug, "OrcaOobSession: gRPC decode error for host {}: {}",
              host_->address()->asString(), status.message());
    stats_.streams_failed_.inc();
    // Count the failure directly here rather than relying on the
    // resetStream() -> onResetStream -> cleanupStream(Failed) chain, which
    // may be deferred by real codecs. cleanupStream(Unsupported) finishes
    // the session-side teardown without double-incrementing. The underlying
    // HTTP/2 stream is not explicitly reset here because scheduleReconnect()
    // will construct a fresh client; any lingering stream on the stale
    // client is torn down when that client is destroyed on the next start().
    cleanupStream(StreamEndReason::Unsupported);
    scheduleReconnect();
    return;
  }
  for (auto& frame : frames) {
    onOrcaLoadReportReceived(std::move(frame.data_));
  }
  if (end_stream) {
    cleanupStream(StreamEndReason::Closed);
    scheduleReconnect();
  }
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
  // A real report round-tripped: the stream is working. Reset the reconnect
  // backoff so the next failure starts from the base bucket. Guarded so we
  // only pay for the reset once per active stream. 200 response headers alone
  // must not trigger this reset — a server that hangs up immediately after
  // headers would otherwise cause a tight reconnect loop.
  if (!backoff_reset_for_active_stream_) {
    backoff_strategy_->reset();
    backoff_reset_for_active_stream_ = true;
  }
  stats_.reports_received_.inc();
  callbacks_.onOrcaOobReport(report);
}

void OrcaOobSession::cleanupStream(StreamEndReason reason) {
  if (!stream_closed_) {
    stats_.active_streams_.dec();
    switch (reason) {
    case StreamEndReason::Failed:
      stats_.streams_failed_.inc();
      break;
    case StreamEndReason::Closed:
      stats_.streams_closed_.inc();
      break;
    case StreamEndReason::Unsupported:
      break;
    }
  }
  stream_closed_ = true;
  request_encoder_ = nullptr;
  grpc_decoder_ = Grpc::Decoder();
}

void OrcaOobSession::handlePermanentFailure(Grpc::Status::WellKnownGrpcStatus status) {
  ENVOY_LOG(debug, "OrcaOobSession: permanent failure for host {}: grpc_status={}",
            host_->address()->asString(), static_cast<int>(status));
  server_supports_oob_ = false;
  if (status == Grpc::Status::WellKnownGrpcStatus::Unimplemented) {
    stats_.backend_not_supported_.inc();
  }
  cleanupStream(StreamEndReason::Unsupported);
  reconnect_timer_->disableTimer();
  if (client_) {
    client_->close(Network::ConnectionCloseType::NoFlush);
    // Defer destruction of the CodecClient: handlePermanentFailure runs from
    // inside an HTTP decode callback on the CodecClient's ActiveRequest, so a
    // synchronous client_.reset() would free the ActiveRequest while the codec
    // wrapper is still accessing `this` (use-after-free in
    // ResponseDecoderWrapper::decodeHeaders' post-callback onDecodeComplete()).
    dispatcher_.deferredDelete(std::move(client_));
  }
  callbacks_.onOrcaOobStreamFailure(status);
}

namespace {
bool isPermanentGrpcFailure(Grpc::Status::WellKnownGrpcStatus status) {
  using G = Grpc::Status::WellKnownGrpcStatus;
  return status == G::Unimplemented || status == G::Unauthenticated ||
         status == G::PermissionDenied || status == G::InvalidArgument;
}
} // namespace

void OrcaOobSession::decodeHeaders(Http::ResponseHeaderMapPtr&& headers, bool end_stream) {
  // See decodeData for the rationale on guarding decode callbacks with
  // stream_closed_. This mirrors that guard for the headers path.
  if (stream_closed_) {
    return;
  }
  const auto http_response_status = Http::Utility::getResponseStatus(*headers);

  if (end_stream) {
    const auto grpc_status = Grpc::Common::getGrpcStatus(*headers);
    if (grpc_status.has_value()) {
      const auto status = static_cast<Grpc::Status::WellKnownGrpcStatus>(grpc_status.value());
      if (isPermanentGrpcFailure(status)) {
        handlePermanentFailure(status);
        return;
      }
    }
  }

  if (http_response_status != 200 || end_stream) {
    cleanupStream(StreamEndReason::Closed);
    scheduleReconnect();
  }
}

void OrcaOobSession::decodeTrailers(Http::ResponseTrailerMapPtr&& trailers) {
  // See decodeData for the rationale on guarding decode callbacks with
  // stream_closed_. This mirrors that guard for the trailers path.
  if (stream_closed_) {
    return;
  }
  auto maybe_grpc_status = Grpc::Common::getGrpcStatus(*trailers);
  if (maybe_grpc_status.has_value()) {
    const auto status = static_cast<Grpc::Status::WellKnownGrpcStatus>(maybe_grpc_status.value());
    if (isPermanentGrpcFailure(status)) {
      handlePermanentFailure(status);
      return;
    }
  }
  cleanupStream(StreamEndReason::Closed);
  scheduleReconnect();
}

void OrcaOobSession::onResetStream(Http::StreamResetReason, absl::string_view) {
  cleanupStream(StreamEndReason::Failed);
  scheduleReconnect();
}

void OrcaOobSession::onEvent(Network::ConnectionEvent event) {
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    cleanupStream(StreamEndReason::Failed);
    scheduleReconnect();
  }
}

void OrcaOobSession::onGoAway(Http::GoAwayErrorCode) {
  // Proactively mark the stream closed and arm the reconnect timer instead of
  // relying on client_->close() to produce a synchronous LocalClose. Real
  // codecs may defer close events to the dispatcher; if the event lagged we
  // would sit idle until something else kicked us. The subsequent re-entrant
  // onEvent(LocalClose) and codec-level stream reset paths are no-ops because
  // cleanupStream() and scheduleReconnect() are both idempotent once the
  // stream is marked closed and the timer is armed.
  cleanupStream(StreamEndReason::Failed);
  scheduleReconnect();
  if (client_) {
    client_->close(Network::ConnectionCloseType::NoFlush);
  }
}

void OrcaOobSession::resetStream() {
  if (request_encoder_ && !stream_closed_) {
    request_encoder_->getStream().resetStream(Http::StreamResetReason::LocalReset);
  }
}

void OrcaOobSession::scheduleReconnect() {
  if (!server_supports_oob_ || stopping_ || replacing_client_) {
    return;
  }
  if (reconnect_timer_->enabled()) {
    return;
  }
  const uint64_t backoff_ms = backoff_strategy_->nextBackOffMs();
  ENVOY_LOG(debug, "OrcaOobSession: scheduling reconnect to {} in {}ms",
            host_->address()->asString(), backoff_ms);
  stats_.reconnect_attempts_.inc();
  reconnect_timer_->enableTimer(std::chrono::milliseconds(backoff_ms));
}

} // namespace Orca
} // namespace Envoy
