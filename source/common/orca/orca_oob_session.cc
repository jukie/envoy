#include "source/common/orca/orca_oob_session.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "envoy/buffer/buffer.h"
#include "envoy/grpc/status.h"
#include "envoy/http/codec.h"
#include "envoy/http/header_map.h"

#include "source/common/buffer/zero_copy_input_stream_impl.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/grpc/common.h"
#include "source/common/grpc/status.h"
#include "source/common/http/codes.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/headers.h"
#include "source/common/http/message_impl.h"
#include "source/common/http/utility.h"

#include "absl/strings/str_cat.h"
#include "xds/service/orca/v3/orca.pb.h"

namespace Envoy {
namespace Orca {

namespace {

constexpr absl::string_view kOrcaOobServiceFullName = "xds.service.orca.v3.OpenRcaService";
constexpr absl::string_view kStreamCoreMetricsMethod = "StreamCoreMetrics";
// Authority used for the gRPC :authority header. This is host-agnostic.
// TODO: plumb the host's hostname / SNI through the factory so the manager
// can pass it here for name-based vhost upstream routing.
constexpr absl::string_view kDefaultAuthority = "orca-oob";

} // namespace

OrcaOobSession::OrcaOobSession(OrcaOobCreateCodecClientCb create_codec_client,
                               Event::Dispatcher& dispatcher,
                               std::chrono::milliseconds report_interval,
                               std::vector<std::string> request_cost_names,
                               bool upstream_secure_transport, OrcaOobReportCb on_report,
                               OrcaOobTerminatedCb on_terminated,
                               OrcaOobLifecycleCb on_lifecycle_event, BackOffStrategyPtr backoff)
    : create_codec_client_(std::move(create_codec_client)), dispatcher_(dispatcher),
      report_interval_(report_interval), request_cost_names_(std::move(request_cost_names)),
      upstream_secure_transport_(upstream_secure_transport), on_report_(std::move(on_report)),
      on_terminated_(std::move(on_terminated)), on_lifecycle_event_(std::move(on_lifecycle_event)),
      backoff_(std::move(backoff)) {
  ASSERT(create_codec_client_ != nullptr);
  ASSERT(on_report_ != nullptr);
  ASSERT(on_terminated_ != nullptr);
  ASSERT(on_lifecycle_event_ != nullptr);
  ASSERT(backoff_ != nullptr);
  retry_timer_ = dispatcher_.createTimer([this]() { connectAndStream(); });
  goaway_drain_timer_ = dispatcher_.createTimer([this]() {
    // Drain deadline elapsed; tear down the connection and reconnect with
    // backoff. Treat as a transient failure so backoff is consulted.
    draining_no_error_goaway_ = false;
    handleTransientFailure("goaway drain deadline expired");
  });
}

OrcaOobSession::~OrcaOobSession() {
  // Suppress callbacks during destruction. Tear down the codec client
  // synchronously here: if the destructor was reached via deferredDelete,
  // we are already on the dispatcher thread between event iterations so a
  // direct destruction is safe.
  stopped_ = true;
  if (retry_timer_) {
    retry_timer_->disableTimer();
  }
  if (goaway_drain_timer_) {
    goaway_drain_timer_->disableTimer();
  }
  if (codec_client_) {
    expect_reset_ = true;
    codec_client_->close(Network::ConnectionCloseType::NoFlush);
    codec_client_.reset();
  }
}

void OrcaOobSession::start() {
  if (started_ || stopped_) {
    return;
  }
  started_ = true;
  ENVOY_LOG(debug, "OrcaOobSession: starting");
  connectAndStream();
}

void OrcaOobSession::close() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  ENVOY_LOG(debug, "OrcaOobSession: close()");
  if (retry_timer_) {
    retry_timer_->disableTimer();
  }
  if (goaway_drain_timer_) {
    goaway_drain_timer_->disableTimer();
  }
  resetStreamState();
  // Tear down via deferredDelete because close() may itself be invoked
  // from inside a callback dispatched on the codec client.
  tearDownCodecClient();
}

void OrcaOobSession::connectAndStream() {
  if (stopped_) {
    return;
  }
  ASSERT(codec_client_ == nullptr);
  resetStreamState();

  codec_client_ = create_codec_client_();
  if (codec_client_ == nullptr) {
    ENVOY_LOG(debug, "OrcaOobSession: codec client factory returned null; scheduling retry");
    handleTransientFailure("codec client factory returned null");
    return;
  }
  codec_client_->addConnectionCallbacks(connection_callback_impl_);
  codec_client_->setCodecConnectionCallbacks(*this);

  request_encoder_ = &codec_client_->newStream(*this);
  request_encoder_->getStream().addCallbacks(*this);

  auto headers_message = Grpc::Common::prepareHeaders(kDefaultAuthority, kOrcaOobServiceFullName,
                                                      kStreamCoreMetricsMethod, absl::nullopt);
  headers_message->headers().setReferenceScheme(upstream_secure_transport_
                                                    ? Http::Headers::get().SchemeValues.Https
                                                    : Http::Headers::get().SchemeValues.Http);
  // The stream is server-streaming with no per-call timeout; the report
  // interval governs frequency.
  auto status = request_encoder_->encodeHeaders(headers_message->headers(), false);
  if (!status.ok()) {
    handleTransientFailure(absl::StrCat("failed to encode headers: ", status.message()));
    return;
  }

  xds::service::orca::v3::OrcaLoadReportRequest request;
  if (report_interval_.count() > 0) {
    auto* duration = request.mutable_report_interval();
    const int64_t seconds = report_interval_.count() / 1000;
    const int32_t nanos = static_cast<int32_t>((report_interval_.count() % 1000) * 1000000);
    duration->set_seconds(seconds);
    duration->set_nanos(nanos);
  }
  for (const auto& name : request_cost_names_) {
    request.add_request_cost_names(name);
  }
  request_encoder_->encodeData(*Grpc::Common::serializeToGrpcFrame(request), true);
}

void OrcaOobSession::scheduleRetry(bool immediate) {
  if (stopped_) {
    return;
  }
  const uint64_t delay_ms = immediate ? 0 : backoff_->nextBackOffMs();
  ENVOY_LOG(debug, "OrcaOobSession: scheduling retry in {} ms (immediate={})", delay_ms, immediate);
  retry_timer_->enableTimer(std::chrono::milliseconds(delay_ms));
}

void OrcaOobSession::tearDownCodecClient() {
  if (codec_client_ == nullptr) {
    return;
  }
  // Move the codec client out before calling close(): this both prevents
  // any synchronously dispatched onConnectionEvent / onResetStream
  // callbacks from observing a still-live codec_client_ pointer (the
  // session early-returns on those callbacks via stopped_/expect_reset_)
  // and ensures we tear it down via deferredDelete so we never destroy a
  // codec client from inside its own callback frame.
  expect_reset_ = true;
  Http::CodecClientPtr to_delete = std::move(codec_client_);
  to_delete->close(Network::ConnectionCloseType::NoFlush);
  dispatcher_.deferredDelete(std::move(to_delete));
}

void OrcaOobSession::resetStreamState() {
  request_encoder_ = nullptr;
  decoder_ = Grpc::Decoder();
  expect_reset_ = false;
  draining_no_error_goaway_ = false;
  backoff_reset_armed_in_attempt_ = false;
}

void OrcaOobSession::handleTransientFailure(absl::string_view reason) {
  if (stopped_ || tearing_down_) {
    return;
  }
  ENVOY_LOG(debug, "OrcaOobSession: transient failure: {}", reason);
  const bool use_immediate_retry = immediate_retry_available_;
  immediate_retry_available_ = false;
  if (goaway_drain_timer_) {
    goaway_drain_timer_->disableTimer();
  }
  tearing_down_ = true;
  resetStreamState();
  tearDownCodecClient();
  tearing_down_ = false;

  // Notify the owner of the transient failure before scheduling the retry.
  on_lifecycle_event_(OrcaOobLifecycleEvent::StreamFailure);
  if (stopped_) {
    // The lifecycle callback may have called close() on us.
    return;
  }
  scheduleRetry(use_immediate_retry);
}

void OrcaOobSession::handleTerminal(Grpc::Status::GrpcStatus status, absl::string_view reason) {
  if (stopped_) {
    return;
  }
  ENVOY_LOG(debug, "OrcaOobSession: terminal status={} reason={}", status, reason);
  // Stop callbacks, drop timers, deferred-delete codec client BEFORE firing
  // on_terminated_, so we never touch members after.
  stopped_ = true;
  if (retry_timer_) {
    retry_timer_->disableTimer();
  }
  if (goaway_drain_timer_) {
    goaway_drain_timer_->disableTimer();
  }
  resetStreamState();
  tearDownCodecClient();

  // Copy the message: it may reference memory owned by a frame the codec
  // client is about to release, so own a string locally.
  const std::string reason_owned(reason);
  // Move the callback locally so members are not touched after we invoke
  // it (the callee is allowed to delete us).
  auto cb = std::move(on_terminated_);
  cb(status, reason_owned);
  // No member access here.
}

void OrcaOobSession::onReport(const xds::data::orca::v3::OrcaLoadReport& report) {
  if (stopped_) {
    return;
  }
  if (!received_any_report_) {
    received_any_report_ = true;
    // First successful report ever: reset backoff and arm the
    // immediate-retry budget so the next failure retries immediately.
    backoff_->reset();
    immediate_retry_available_ = true;
  }
  if (!backoff_reset_armed_in_attempt_) {
    backoff_reset_armed_in_attempt_ = true;
    // Reset backoff on every successful attempt, not just the first one,
    // so that a string of healthy connections keeps the retry budget
    // ready even if there is an earlier transient blip.
    backoff_->reset();
  }
  on_report_(report);
}

void OrcaOobSession::onRpcComplete(Grpc::Status::GrpcStatus status, absl::string_view message,
                                   bool end_stream) {
  if (stopped_) {
    return;
  }
  if (status == Grpc::Status::WellKnownGrpcStatus::Unimplemented) {
    handleTerminal(status, message);
    return;
  }
  // Any other completion is treated as a transient failure: the server
  // ended the stream and we should reconnect.
  if (!end_stream && request_encoder_ != nullptr) {
    expect_reset_ = true;
    request_encoder_->getStream().resetStream(Http::StreamResetReason::LocalReset);
  }
  handleTransientFailure(absl::StrCat("rpc complete: status=", status, " ", message));
}

void OrcaOobSession::decodeHeaders(Http::ResponseHeaderMapPtr&& headers, bool end_stream) {
  if (stopped_) {
    return;
  }
  const auto http_response_status = Http::Utility::getResponseStatus(*headers);
  if (http_response_status != enumToInt(Http::Code::OK)) {
    if (end_stream) {
      const auto grpc_status = Grpc::Common::getGrpcStatus(*headers);
      if (grpc_status) {
        onRpcComplete(grpc_status.value(), Grpc::Common::getGrpcMessage(*headers), true);
        return;
      }
    }
    onRpcComplete(Grpc::Utility::httpToGrpcStatus(http_response_status), "non-200 HTTP response",
                  end_stream);
    return;
  }
  if (!Grpc::Common::isGrpcResponseHeaders(*headers, end_stream)) {
    onRpcComplete(Grpc::Status::WellKnownGrpcStatus::Internal, "not a gRPC response", false);
    return;
  }
  if (end_stream) {
    // Trailers-only response.
    const auto grpc_status = Grpc::Common::getGrpcStatus(*headers);
    if (grpc_status) {
      onRpcComplete(grpc_status.value(), Grpc::Common::getGrpcMessage(*headers), true);
      return;
    }
    onRpcComplete(Grpc::Status::WellKnownGrpcStatus::Internal,
                  "gRPC protocol violation: unexpected stream end", true);
    return;
  }
  // Headers-only success: the stream is open. Fire the lifecycle event.
  on_lifecycle_event_(OrcaOobLifecycleEvent::StreamOpen);
}

void OrcaOobSession::decodeData(Buffer::Instance& data, bool end_stream) {
  if (stopped_) {
    return;
  }
  std::vector<Grpc::Frame> decoded_frames;
  if (!decoder_.decode(data, decoded_frames).ok()) {
    onRpcComplete(Grpc::Status::WellKnownGrpcStatus::Internal, "gRPC wire protocol decode error",
                  false);
    return;
  }
  for (auto& frame : decoded_frames) {
    if (frame.length_ == 0) {
      continue;
    }
    xds::data::orca::v3::OrcaLoadReport report;
    Buffer::ZeroCopyInputStreamImpl stream(std::move(frame.data_));
    if (frame.flags_ != Grpc::GRPC_FH_DEFAULT || !report.ParseFromZeroCopyStream(&stream)) {
      onRpcComplete(Grpc::Status::WellKnownGrpcStatus::Internal,
                    "invalid xds.data.orca.v3.OrcaLoadReport payload", false);
      return;
    }
    onReport(report);
    if (stopped_) {
      // The user callback (or a chained transition) closed/terminated us.
      return;
    }
  }
  if (end_stream) {
    onRpcComplete(Grpc::Status::WellKnownGrpcStatus::Unknown,
                  "server ended stream without trailers", true);
  }
}

void OrcaOobSession::decodeTrailers(Http::ResponseTrailerMapPtr&& trailers) {
  if (stopped_) {
    return;
  }
  auto maybe_grpc_status = Grpc::Common::getGrpcStatus(*trailers);
  auto grpc_status =
      maybe_grpc_status
          ? maybe_grpc_status.value()
          : static_cast<Grpc::Status::GrpcStatus>(Grpc::Status::WellKnownGrpcStatus::Internal);
  const std::string grpc_message =
      maybe_grpc_status ? Grpc::Common::getGrpcMessage(*trailers) : "invalid gRPC status";
  onRpcComplete(grpc_status, grpc_message, true);
}

void OrcaOobSession::onResetStream(Http::StreamResetReason reason,
                                   absl::string_view transport_failure_reason) {
  if (stopped_ || tearing_down_) {
    return;
  }
  const bool expected = expect_reset_;
  expect_reset_ = false;
  if (expected) {
    // We initiated the reset (e.g. from onRpcComplete or close()); state
    // teardown has already happened.
    return;
  }
  ENVOY_LOG(debug, "OrcaOobSession: stream reset reason={} ({})", static_cast<int>(reason),
            transport_failure_reason);
  handleTransientFailure(absl::StrCat("stream reset: ", transport_failure_reason));
}

void OrcaOobSession::onGoAway(Http::GoAwayErrorCode error_code) {
  if (stopped_ || tearing_down_) {
    return;
  }
  ENVOY_LOG(debug, "OrcaOobSession: GOAWAY error_code={}", static_cast<int>(error_code));
  if (error_code == Http::GoAwayErrorCode::NoError) {
    if (draining_no_error_goaway_) {
      // Already draining; nothing to do.
      return;
    }
    draining_no_error_goaway_ = true;
    goaway_drain_timer_->enableTimer(kGoAwayDrainDeadline);
    return;
  }
  // Other GOAWAY: treat as transient failure and reconnect.
  handleTransientFailure(absl::StrCat("GOAWAY error code ", static_cast<int>(error_code)));
}

void OrcaOobSession::onConnectionEvent(Network::ConnectionEvent event) {
  if (stopped_ || tearing_down_) {
    return;
  }
  if (event == Network::ConnectionEvent::Connected ||
      event == Network::ConnectionEvent::ConnectedZeroRtt) {
    return;
  }
  ENVOY_LOG(debug, "OrcaOobSession: connection event {} during attempt", static_cast<int>(event));
  handleTransientFailure("connection closed");
}

} // namespace Orca
} // namespace Envoy
