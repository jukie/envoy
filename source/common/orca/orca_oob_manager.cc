#include "source/common/orca/orca_oob_manager.h"

#include <chrono>
#include <memory>

#include "source/common/network/address_impl.h"
#include "source/common/network/utility.h"
#include "source/common/orca/orca_load_metrics.h"
#include "source/common/stream_info/stream_info_impl.h"

#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Orca {

OrcaOobManager::OrcaOobManager(const Upstream::PrioritySet& priority_set,
                               Event::Dispatcher& dispatcher, Random::RandomGenerator& random,
                               std::chrono::milliseconds reporting_period,
                               std::chrono::milliseconds initial_jitter,
                               Stats::Scope& stats_scope,
                               OptRef<const std::vector<std::string>> lrs_metric_names)
    : priority_set_(priority_set), dispatcher_(dispatcher), random_(random),
      reporting_period_(reporting_period), initial_jitter_(initial_jitter),
      stats_({ALL_ORCA_OOB_STATS(POOL_COUNTER_PREFIX(stats_scope, "orca_oob."),
                                 POOL_GAUGE_PREFIX(stats_scope, "orca_oob."))}),
      lrs_metric_names_(lrs_metric_names) {}

OrcaOobManager::~OrcaOobManager() { stop(); }

void OrcaOobManager::start() {
  // Register priority update callback. This MUST happen after the LB's own
  // callback registration so that LB policy data is attached before OOB sessions start.
  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t, const Upstream::HostVector& hosts_added,
             const Upstream::HostVector& hosts_removed) {
        onHostsAdded(hosts_added);
        onHostsRemoved(hosts_removed);
      });

  // Create sessions for all existing hosts.
  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    onHostsAdded(host_set->hosts());
  }
}

void OrcaOobManager::stop() {
  priority_update_cb_.reset();
  for (auto& [host, session] : sessions_) {
    if (session->jitter_timer_) {
      session->jitter_timer_->disableTimer();
    }
    session->session_->stop();
  }
  sessions_.clear();
}

void OrcaOobManager::onHostsAdded(const Upstream::HostVector& hosts_added) {
  for (const auto& host : hosts_added) {
    if (sessions_.contains(host)) {
      continue;
    }
    auto session = std::make_unique<HostSession>(host, dispatcher_, random_, reporting_period_,
                                                 lrs_metric_names_, stats_);
    startSessionWithJitter(*session);
    sessions_.emplace(host, std::move(session));
  }
}

void OrcaOobManager::onHostsRemoved(const Upstream::HostVector& hosts_removed) {
  for (const auto& host : hosts_removed) {
    auto it = sessions_.find(host);
    if (it != sessions_.end()) {
      if (it->second->jitter_timer_) {
        it->second->jitter_timer_->disableTimer();
      }
      it->second->session_->stop();
      sessions_.erase(it);
    }
  }
}

void OrcaOobManager::startSessionWithJitter(HostSession& session) {
  if (initial_jitter_.count() > 0) {
    const auto jitter_ms =
        std::chrono::milliseconds(random_.random() % initial_jitter_.count());
    session.jitter_timer_ =
        dispatcher_.createTimer([&session]() { session.session_->start(); });
    session.jitter_timer_->enableTimer(jitter_ms);
  } else {
    session.session_->start();
  }
}

OrcaOobManager::HostSession::HostSession(
    Upstream::HostSharedPtr host, Event::Dispatcher& dispatcher, Random::RandomGenerator& random,
    std::chrono::milliseconds reporting_period,
    OptRef<const std::vector<std::string>> lrs_metric_names, OrcaOobStats& stats)
    : host_(host), lrs_metric_names_(lrs_metric_names), dispatcher_(dispatcher) {
  session_ =
      std::make_unique<OrcaOobSession>(host_, dispatcher, random, reporting_period, *this, stats);
}

void OrcaOobManager::HostSession::onOrcaOobReport(
    const xds::data::orca::v3::OrcaLoadReport& report) {
  // 1. Deliver to LRS if configured.
  if (lrs_metric_names_.has_value()) {
    Envoy::Orca::addOrcaLoadReportToLoadMetricStats(*lrs_metric_names_, report,
                                                    host_->loadMetricStats());
  }

  // 2. Deliver to LB policy for weight calculation.
  auto lb_data = host_->lbPolicyData();
  if (lb_data.has_value()) {
    // ClientSideHostLbPolicyData::onOrcaLoadReport() ignores the StreamInfo parameter
    // -- it only uses the OrcaLoadReport for weight calculation. Construct a minimal
    // synthetic StreamInfo to satisfy the interface.
    auto connection_info = std::make_shared<Network::ConnectionInfoSetterImpl>(
        Network::Utility::getCanonicalIpv4LoopbackAddress(),
        Network::Utility::getCanonicalIpv4LoopbackAddress());
    StreamInfo::StreamInfoImpl stream_info(Http::Protocol::Http2, dispatcher_.timeSource(),
                                           connection_info,
                                           StreamInfo::FilterState::LifeSpan::FilterChain);
    const absl::Status status = lb_data->onOrcaLoadReport(report, stream_info);
    if (!status.ok()) {
      ENVOY_LOG(debug, "OrcaOobManager: LB policy onOrcaLoadReport failed for host {}: {}",
                host_->address()->asString(), status.message());
    }
  }
}

void OrcaOobManager::HostSession::onOrcaOobStreamFailure(Grpc::Status::GrpcStatus status) {
  ENVOY_LOG(debug, "OrcaOobManager: OOB stream failure for host {}, status: {}",
            host_->address()->asString(), static_cast<int>(status));
}

} // namespace Orca
} // namespace Envoy
