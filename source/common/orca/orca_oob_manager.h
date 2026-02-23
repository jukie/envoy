#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/random_generator.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/stats/scope.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/callback_impl.h"
#include "source/common/common/logger.h"
#include "source/common/orca/orca_oob_session.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Orca {

/**
 * Manages the set of OrcaOobSession instances across all hosts in a cluster,
 * reacting to host membership changes.
 */
class OrcaOobManager : Logger::Loggable<Logger::Id::upstream> {
public:
  OrcaOobManager(const Upstream::PrioritySet& priority_set, Event::Dispatcher& dispatcher,
                 Random::RandomGenerator& random, std::chrono::milliseconds reporting_period,
                 std::chrono::milliseconds initial_jitter, Stats::Scope& stats_scope,
                 OptRef<const std::vector<std::string>> lrs_metric_names);
  ~OrcaOobManager();

  // Start OOB reporting for all current and future hosts.
  void start();

  // Stop all sessions and unregister callbacks.
  void stop();

private:
  // Per-host session wrapper implementing OrcaOobCallbacks.
  class HostSession : public OrcaOobCallbacks {
  public:
    HostSession(Upstream::HostSharedPtr host, Event::Dispatcher& dispatcher,
                Random::RandomGenerator& random, std::chrono::milliseconds reporting_period,
                OptRef<const std::vector<std::string>> lrs_metric_names, OrcaOobStats& stats);

    void onOrcaOobReport(const xds::data::orca::v3::OrcaLoadReport& report) override;
    void onOrcaOobStreamFailure(Grpc::Status::GrpcStatus status) override;

    std::unique_ptr<OrcaOobSession> session_;
    Upstream::HostSharedPtr host_;
    Event::TimerPtr jitter_timer_;
    OptRef<const std::vector<std::string>> lrs_metric_names_;

  private:
    Event::Dispatcher& dispatcher_;
  };

  // Called when hosts are added/removed from the priority set.
  void onHostsAdded(const Upstream::HostVector& hosts_added);
  void onHostsRemoved(const Upstream::HostVector& hosts_removed);

  // Start a session with optional jitter delay.
  void startSessionWithJitter(HostSession& session);

  const Upstream::PrioritySet& priority_set_;
  Event::Dispatcher& dispatcher_;
  Random::RandomGenerator& random_;
  std::chrono::milliseconds reporting_period_;
  std::chrono::milliseconds initial_jitter_;
  OrcaOobStats stats_;
  OptRef<const std::vector<std::string>> lrs_metric_names_;

  absl::flat_hash_map<Upstream::HostSharedPtr, std::unique_ptr<HostSession>> sessions_;
  Common::CallbackHandlePtr priority_update_cb_;
};

} // namespace Orca
} // namespace Envoy
