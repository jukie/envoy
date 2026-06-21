#include "source/extensions/load_balancing_policies/load_aware_locality/load_aware_locality_lb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>

#include "envoy/stats/stats_macros.h"

#include "source/common/protobuf/utility.h"
#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"
#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace LoadAwareLocality {

absl::Status LocalityLbHostData::onOrcaLoadReport(const Upstream::OrcaLoadReport& report,
                                                  const StreamInfo::StreamInfo&) {
  const double util =
      Common::OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, metric_names_);
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             time_source_.monotonicTime().time_since_epoch())
                             .count();
  storeUtilization(util, now_ms);
  return absl::OkStatus();
}

// --- LoadAwareLocalityLoadBalancer (main thread) ---

LoadAwareLocalityLoadBalancer::LoadAwareLocalityLoadBalancer(
    OptRef<const Upstream::LoadBalancerConfig> lb_config, const Upstream::ClusterInfo& cluster_info,
    const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
    Envoy::Random::RandomGenerator& random, TimeSource& time_source)
    : priority_set_(priority_set), stats_(cluster_info.lbStats()),
      lb_stats_{ALL_LOAD_AWARE_LOCALITY_STATS(
          POOL_COUNTER_PREFIX(cluster_info.statsScope(), "load_aware_locality"))},
      time_source_(time_source) {
  const auto* typed_config = dynamic_cast<const LoadAwareLocalityLbConfig*>(lb_config.ptr());
  ASSERT(typed_config != nullptr);

  utilization_variance_threshold_ = typed_config->utilizationVarianceThreshold();
  ewma_alpha_ = typed_config->ewmaAlpha();
  remote_probe_fraction_ = typed_config->remoteProbeFraction();
  weight_expiration_period_ = typed_config->weightExpirationPeriod();
  weight_update_period_ = typed_config->weightUpdatePeriod();
  metric_names_ = typed_config->metricNamesForComputingUtilization();

  factory_ = std::make_shared<WorkerLocalLbFactory>(
      typed_config->endpointPickingPolicyFactory(), typed_config->endpointPickingPolicyName(),
      typed_config->endpointPickingPolicyConfig(), cluster_info, priority_set, runtime, random,
      time_source, typed_config->tlsSlotAllocator());

  weight_update_timer_ = typed_config->mainThreadDispatcher().createTimer(
      [this]() { computeLocalityRoutingWeights(); });
}

LoadAwareLocalityLoadBalancer::~LoadAwareLocalityLoadBalancer() = default;

void LoadAwareLocalityLoadBalancer::addLbPolicyDataToHosts(const Upstream::HostVector& hosts) {
  for (const auto& host_ptr : hosts) {
    if (!host_ptr->typedLbPolicyData<LocalityLbHostData>().has_value()) {
      auto data = std::make_unique<LocalityLbHostData>(time_source_, metric_names_);
      host_ptr->addLbPolicyData(std::move(data));
    }
  }
}

absl::Status LoadAwareLocalityLoadBalancer::initialize() {
  RETURN_IF_NOT_OK(factory_->initializeChildLb());

  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    addLbPolicyDataToHosts(host_set->hosts());
  }

  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t, const Upstream::HostVector& hosts_added, const Upstream::HostVector&) {
        addLbPolicyDataToHosts(hosts_added);
      });

  computeLocalityRoutingWeights();
  return absl::OkStatus();
}

void LoadAwareLocalityLoadBalancer::computeLocalityRoutingWeights() {
  // Re-arm first so the timer always fires on schedule regardless of early returns below.
  weight_update_timer_->enableTimer(weight_update_period_);
  stats_.lb_recalculate_zone_structures_.inc();

  auto snapshot = std::make_shared<RoutingWeightsSnapshot>();
  const auto& host_sets = priority_set_.hostSetsPerPriority();
  snapshot->priority_weights.resize(host_sets.size());

  for (auto& per_source_smoothed : smoothed_utilizations_) {
    per_source_smoothed.resize(host_sets.size());
  }
  for (auto& per_source_valid : smoothed_utilizations_valid_) {
    per_source_valid.resize(host_sets.size());
  }

  // Current monotonic time in milliseconds, used for weight expiration checks.
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             time_source_.monotonicTime().time_since_epoch())
                             .count();

  // Tick-scoped stat accumulators: OR'd across every (source × priority) pass below.
  bool tick_all_overloaded = false;
  bool tick_local_preferred = false;
  bool tick_probe_active = false;
  uint32_t tick_stale_localities = 0; // summed only from the all_hosts pass

  for (size_t priority = 0; priority < host_sets.size(); ++priority) {
    const auto& host_set = host_sets[priority];
    const auto& hosts_per_locality = host_set->hostsPerLocality();
    const auto& locality_hosts = hosts_per_locality.get();
    auto& priority_snapshot = snapshot->priority_weights[priority];

    if (locality_hosts.empty()) {
      for (size_t source = 0; source < smoothed_utilizations_.size(); ++source) {
        smoothed_utilizations_[source][priority].clear();
        smoothed_utilizations_valid_[source][priority].clear();
      }
      continue;
    }

    priority_snapshot.has_local_locality = hosts_per_locality.hasLocalLocality();
    using SelectionSource = PriorityRoutingWeights::SelectionSource;
    const auto run_source = [&](SelectionSource src,
                                const std::vector<Upstream::HostVector>& eligible_hosts) {
      const size_t s = static_cast<size_t>(src);
      auto& sw = priority_snapshot.by_source[s];
      return computeSourceWeights(hosts_per_locality, eligible_hosts, now_ms, sw.weights,
                                  sw.all_local, smoothed_utilizations_[s][priority],
                                  smoothed_utilizations_valid_[s][priority]);
    };
    const auto healthy_result =
        run_source(SelectionSource::Healthy, host_set->healthyHostsPerLocality().get());
    const auto degraded_result =
        run_source(SelectionSource::Degraded, host_set->degradedHostsPerLocality().get());
    // all_hosts pass: run last so its staleness reflects the canonical "zero fresh hosts".
    const auto all_hosts_result = run_source(SelectionSource::AllHosts, locality_hosts);

    tick_all_overloaded |= healthy_result.all_overloaded || degraded_result.all_overloaded ||
                           all_hosts_result.all_overloaded;
    tick_local_preferred |= healthy_result.local_preferred || degraded_result.local_preferred ||
                            all_hosts_result.local_preferred;
    tick_probe_active |= healthy_result.probe_active || degraded_result.probe_active ||
                         all_hosts_result.probe_active;
    // Count stale localities from the all_hosts pass only (canonical; avoids triple-counting).
    tick_stale_localities += all_hosts_result.stale_localities;
  }

  if (tick_all_overloaded) {
    lb_stats_.all_overloaded_total_.inc();
  }
  if (tick_local_preferred) {
    lb_stats_.local_preferred_total_.inc();
  }
  if (tick_probe_active) {
    lb_stats_.probe_active_total_.inc();
  }
  if (tick_stale_localities > 0) {
    lb_stats_.stale_locality_total_.add(tick_stale_localities);
  }

  ENVOY_LOG(trace, "computeLocalityRoutingWeights: {} priorities",
            snapshot->priority_weights.size());
  factory_->updateRoutingWeights(std::move(snapshot));
}

LoadAwareLocalityLoadBalancer::SourceComputeResult
LoadAwareLocalityLoadBalancer::computeSourceWeights(
    const Upstream::HostsPerLocality& all_hosts_per_locality,
    const std::vector<Upstream::HostVector>& eligible_hosts_per_locality, int64_t now_ms,
    LocalityWeightsMap& weights_map, bool& all_local, std::vector<double>& smoothed,
    std::vector<bool>& smoothed_valid) {
  SourceComputeResult result;
  const auto& locality_hosts = all_hosts_per_locality.get();
  const size_t locality_count = locality_hosts.size();
  // Compute weights index-based (local locality is index 0), then key by identity at the end.
  std::vector<double> weights(locality_count, 0.0);
  all_local = false;

  std::vector<double> avg_utils(locality_count, 0.0);
  std::vector<uint32_t> valid_counts(locality_count, 0);
  std::vector<uint32_t> host_counts(locality_count, 0);

  for (size_t i = 0; i < locality_count; ++i) {
    const bool has_eligible = i < eligible_hosts_per_locality.size();
    host_counts[i] =
        has_eligible ? static_cast<uint32_t>(eligible_hosts_per_locality[i].size()) : 0u;

    double util_sum = 0.0;
    uint32_t valid_count = 0;
    if (has_eligible) {
      for (const auto& host : eligible_hosts_per_locality[i]) {
        auto host_data = host->typedLbPolicyData<LocalityLbHostData>();
        if (!host_data.has_value()) {
          continue;
        }
        const int64_t last_update_ms = host_data->lastUpdateTimeMs();
        if (last_update_ms == 0) {
          continue;
        }

        if (weight_expiration_period_.count() > 0 &&
            (now_ms - last_update_ms) > weight_expiration_period_.count()) {
          continue;
        }

        util_sum += host_data->utilization();
        valid_count++;
      }
    }

    avg_utils[i] = valid_count > 0 ? util_sum / valid_count : 0.0;
    valid_counts[i] = valid_count;
  }

  if (smoothed.size() != locality_count || smoothed_valid.size() != locality_count) {
    smoothed.assign(locality_count, 0.0);
    smoothed_valid.assign(locality_count, false);
  }

  std::vector<double> utilizations(locality_count, 0.0);
  std::vector<bool> stale(locality_count, false);
  for (size_t i = 0; i < locality_count; ++i) {
    if (valid_counts[i] > 0) {
      if (!smoothed_valid[i]) {
        smoothed[i] = avg_utils[i];
        smoothed_valid[i] = true;
      } else {
        smoothed[i] = ewma_alpha_ * avg_utils[i] + (1.0 - ewma_alpha_) * smoothed[i];
      }
    } else {
      // All hosts stale/missing: carry the prior smoothed value (0 only at cold start) so a
      // locality whose load we no longer know is not mistaken for idle. The locality falls
      // back to its host-count baseline below.
      stale[i] = true;
    }
    utilizations[i] = smoothed[i];
  }

  uint32_t total_hosts = 0;
  for (size_t i = 0; i < locality_count; ++i) {
    weights[i] = stale[i] ? static_cast<double>(host_counts[i])
                          : host_counts[i] * std::max(0.0, 1.0 - utilizations[i]);
    total_hosts += host_counts[i];
  }

  const auto set_all_local = [&weights, &all_local]() {
    all_local = true;
    std::fill(weights.begin(), weights.end(), 0.0);
    if (!weights.empty()) {
      weights[0] = 1.0;
    }
  };

  // Remote host count, shared by the local-preference target and the probe redistribution below.
  uint32_t remote_hosts = 0;
  if (all_hosts_per_locality.hasLocalLocality()) {
    double remote_util_sum = 0.0;
    for (size_t i = 1; i < locality_count; ++i) {
      remote_util_sum += utilizations[i] * host_counts[i];
      remote_hosts += host_counts[i];
    }

    if (total_hosts > 0 && !weights.empty() && weights[0] > 0.0) {
      const double target_util = remote_hosts > 0 ? remote_util_sum / remote_hosts : 0.0;
      if (utilizations[0] <= target_util + utilization_variance_threshold_) {
        set_all_local();
        // Only count the meaningful variance snap (not the degenerate empty-host fallback).
        result.local_preferred = true;
      }
    } else if (total_hosts == 0) {
      set_all_local();
    }
  }

  if (remote_hosts > 0 && remote_probe_fraction_ > 0.0) {
    const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
    const double remote_target = total * remote_probe_fraction_;
    const double remote_sum = total - weights[0];
    if (total > 0.0 && remote_sum < remote_target) {
      const double take_from_local = std::min(remote_target - remote_sum, weights[0]);
      weights[0] -= take_from_local;
      for (size_t i = 1; i < locality_count; ++i) {
        weights[i] += take_from_local * static_cast<double>(host_counts[i]) / remote_hosts;
      }
      result.probe_active = true;
    }
  }

  const double total_weight = std::accumulate(weights.begin(), weights.end(), 0.0);
  if (total_weight == 0.0 && total_hosts > 0) {
    for (size_t i = 0; i < locality_count; ++i) {
      weights[i] = static_cast<double>(host_counts[i]);
    }
    result.all_overloaded = true;
  }

  // Key the computed weights by Locality identity. An empty locality group carries no entry
  // (it has 0 weight anyway); the local locality is stored under its own identity like the
  // rest (local-vs-remote preference is already baked into the weight values).
  weights_map.clear();
  weights_map.reserve(locality_count);
  for (size_t i = 0; i < locality_count; ++i) {
    if (locality_hosts[i].empty()) {
      continue;
    }
    weights_map[locality_hosts[i][0]->locality()] = weights[i];
  }

  for (size_t i = 0; i < locality_count; ++i) {
    if (stale[i]) {
      ++result.stale_localities;
    }
  }
  return result;
}

// --- WorkerLocalLbFactory ---

WorkerLocalLbFactory::WorkerLocalLbFactory(
    Upstream::TypedLoadBalancerFactory& child_factory, std::string child_factory_name,
    LoadBalancerConfigSharedPtr child_config, const Upstream::ClusterInfo& cluster_info,
    const Upstream::PrioritySet& cluster_priority_set, Runtime::Loader& runtime,
    Envoy::Random::RandomGenerator& random, TimeSource& time_source,
    ThreadLocal::SlotAllocator& tls_slot_allocator)
    : child_factory_name_(std::move(child_factory_name)), child_config_(std::move(child_config)),
      cluster_info_(cluster_info), random_(random), runtime_(runtime),
      healthy_panic_threshold_(PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
          cluster_info.lbConfig(), healthy_panic_threshold, 100, 50)),
      fail_traffic_on_panic_(
          cluster_info.lbConfig().zone_aware_lb_config().fail_traffic_on_panic()) {
  auto child_config_ref =
      makeOptRefFromPtr<const Upstream::LoadBalancerConfig>(child_config_.get());
  child_thread_aware_lb_ = child_factory.create(
      child_config_ref, cluster_info_, cluster_priority_set, runtime, random_, time_source);
  tls_ = ThreadLocal::TypedSlot<ThreadLocalShim>::makeUnique(tls_slot_allocator);
  tls_->set([](Event::Dispatcher&) { return std::make_shared<ThreadLocalShim>(); });
}

absl::Status WorkerLocalLbFactory::initializeChildLb() {
  if (child_thread_aware_lb_ == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Unsupported endpoint picking policy for load_aware_locality: ", child_factory_name_,
        ". Child load balancer could not be instantiated per locality."));
  }
  return child_thread_aware_lb_->initialize();
}

Upstream::LoadBalancerPtr
WorkerLocalLbFactory::createWorkerChildLb(Upstream::PrioritySetImpl& per_locality_priority_set) {
  // initializeChildLb() must have been called on the main thread before workers call this.
  ASSERT(child_thread_aware_lb_ != nullptr);
  Upstream::LoadBalancerParams child_params{per_locality_priority_set, nullptr};
  return child_thread_aware_lb_->factory()->create(child_params);
}

bool WorkerLocalLbFactory::recreateChildOnHostChange() const {
  ASSERT(child_thread_aware_lb_ != nullptr);
  return child_thread_aware_lb_->factory()->recreateOnHostChangeDeprecated();
}

Upstream::LoadBalancerPtr WorkerLocalLbFactory::create(Upstream::LoadBalancerParams params) {
  return std::make_unique<WorkerLocalLb>(*this, params.priority_set);
}

// --- WorkerLocalLb (per-worker) ---

WorkerLocalLb::WorkerLocalLb(WorkerLocalLbFactory& factory,
                             const Upstream::PrioritySet& priority_set)
    : Upstream::LoadBalancerBase(priority_set, factory.lbStats(), factory.runtime(),
                                 factory.random(), factory.healthyPanicThreshold()),
      factory_(factory) {
  buildPerPriorityLocalities();
  // Register AFTER initial build so callback doesn't fire during construction.
  member_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t priority, const Upstream::HostVector& hosts_added,
             const Upstream::HostVector& hosts_removed) {
        // A membership delta permits a topology rebuild. An empty-delta update still matters for
        // child policies (in-place host attribute changes such as health, weight, or metadata) but
        // never rebuilds — a real topology change always arrives with a delta.
        syncPriority(priority, /*allow_rebuild=*/!hosts_added.empty() || !hosts_removed.empty());
      });
}

WorkerLocalLb::~WorkerLocalLb() {
  // Reset callback handle before other members are destroyed, so the callback
  // doesn't fire during destruction and access freed per-locality state.
  member_update_cb_.reset();
}

void WorkerLocalLb::updateLocalityHosts(PerSourceLocalityState& state,
                                        const Upstream::HostVector& hosts, bool is_local,
                                        const Upstream::HostVector& hosts_added,
                                        const Upstream::HostVector& hosts_removed) {
  auto hosts_shared = std::make_shared<Upstream::HostVector>(hosts);
  auto per_locality = std::make_shared<Upstream::HostsPerLocalityImpl>(hosts, is_local);
  // All passed-in hosts are marked as "healthy" in the child priority set regardless of their
  // actual health status. The caller (syncLocalityState) already partitions hosts by source
  // (healthy/degraded/all), so this child LB only sees hosts that can be selected. Marking
  // them all healthy ensures the child policy (e.g. RoundRobin) considers every host eligible.
  auto healthy_hosts = std::make_shared<const Upstream::HealthyHostVector>(hosts);
  auto update_params = Upstream::HostSetImpl::updateHostsParams(
      hosts_shared, per_locality, healthy_hosts, per_locality,
      std::make_shared<const Upstream::DegradedHostVector>(),
      Upstream::HostsPerLocalityImpl::empty(),
      std::make_shared<const Upstream::ExcludedHostVector>(),
      Upstream::HostsPerLocalityImpl::empty());
  state.priority_set->updateHosts(0, std::move(update_params), nullptr, hosts_added, hosts_removed,
                                  absl::nullopt, absl::nullopt);
}

void WorkerLocalLb::buildPerPriorityLocalities() {
  const auto& host_sets = priority_set_.hostSetsPerPriority();
  per_priority_locality_.clear();
  per_priority_locality_.resize(host_sets.size());

  for (size_t priority = 0; priority < host_sets.size(); ++priority) {
    buildPerLocality(priority, *host_sets[priority]);
  }
}

void WorkerLocalLb::syncLocalityState(PerLocalityState& state, const Upstream::HostSet& host_set,
                                      size_t locality_index, bool recreate_child) {
  static const Upstream::HostVector empty_hosts;
  const auto& all_localities = host_set.hostsPerLocality().get();
  const auto& healthy_localities = host_set.healthyHostsPerLocality().get();
  const auto& degraded_localities = host_set.degradedHostsPerLocality().get();
  const bool is_local = (locality_index == 0 && host_set.hostsPerLocality().hasLocalLocality());

  const auto& all_hosts = all_localities[locality_index];
  const auto& healthy_hosts =
      locality_index < healthy_localities.size() ? healthy_localities[locality_index] : empty_hosts;
  const auto& degraded_hosts = locality_index < degraded_localities.size()
                                   ? degraded_localities[locality_index]
                                   : empty_hosts;

  // Record this locality's identity for chooseLocality's weight lookup. Empty group → no identity.
  state.locality =
      all_hosts.empty() ? envoy::config::core::v3::Locality() : all_hosts[0]->locality();

  const auto sync_source = [this, is_local, recreate_child](PerSourceLocalityState& source_state,
                                                            const Upstream::HostVector& new_hosts) {
    if (new_hosts.empty()) {
      if (source_state.priority_set != nullptr) {
        source_state.lb.reset();
        source_state.priority_set.reset();
      }
      return;
    }

    if (source_state.priority_set == nullptr) {
      source_state.priority_set = std::make_unique<Upstream::PrioritySetImpl>();
      updateLocalityHosts(source_state, new_hosts, is_local, new_hosts, {});
      source_state.lb = factory_.createWorkerChildLb(*source_state.priority_set);
      return;
    }

    const auto& old_hosts = source_state.priority_set->hostSetsPerPriority()[0]->hosts();
    absl::flat_hash_set<Upstream::HostConstSharedPtr> old_set(old_hosts.begin(), old_hosts.end());

    Upstream::HostVector hosts_added;
    Upstream::HostVector hosts_removed;
    for (const auto& host : new_hosts) {
      if (!old_set.erase(host)) {
        hosts_added.push_back(host);
      }
    }
    hosts_removed.reserve(old_set.size());
    for (const auto& host : old_set) {
      hosts_removed.push_back(std::const_pointer_cast<Upstream::Host>(host));
    }

    const bool membership_changed = !hosts_added.empty() || !hosts_removed.empty();
    if (!membership_changed) {
      // Host identity is unchanged, but child policies still need an update to observe in-place
      // host attribute changes such as weight or metadata.
      updateLocalityHosts(source_state, new_hosts, is_local, {}, {});
      return;
    }

    updateLocalityHosts(source_state, new_hosts, is_local, hosts_added, hosts_removed);
    if (recreate_child) {
      source_state.lb = factory_.createWorkerChildLb(*source_state.priority_set);
    }
  };

  sync_source(state.stateFor(PriorityRoutingWeights::SelectionSource::Healthy), healthy_hosts);
  sync_source(state.stateFor(PriorityRoutingWeights::SelectionSource::Degraded), degraded_hosts);
  sync_source(state.stateFor(PriorityRoutingWeights::SelectionSource::AllHosts), all_hosts);
}

void WorkerLocalLb::buildPerLocality(uint32_t priority, const Upstream::HostSet& host_set) {
  const auto& locality_hosts = host_set.hostsPerLocality().get();
  auto& per_locality = per_priority_locality_[priority].localities;
  per_locality.clear();
  per_locality.resize(locality_hosts.size());

  for (size_t i = 0; i < locality_hosts.size(); ++i) {
    syncLocalityState(per_locality[i], host_set, i, /*recreate_child=*/false);
  }
}

void WorkerLocalLb::syncPriority(uint32_t priority, bool allow_rebuild) {
  const auto& host_sets = priority_set_.hostSetsPerPriority();
  if (priority >= host_sets.size()) {
    return;
  }

  // Priority count changed — full rebuild when permitted, else wait for a membership-delta update.
  if (host_sets.size() != per_priority_locality_.size()) {
    if (allow_rebuild) {
      buildPerPriorityLocalities();
    }
    return;
  }

  const auto& host_set = host_sets[priority];
  const auto& locality_hosts = host_set->hostsPerLocality().get();
  auto& per_locality = per_priority_locality_[priority].localities;

  // Topology change (locality added/removed) — rebuild this priority when permitted, else wait.
  if (locality_hosts.size() != per_locality.size()) {
    if (allow_rebuild) {
      buildPerLocality(priority, *host_set);
    }
    return;
  }

  const bool recreate_child = factory_.recreateChildOnHostChange();
  for (size_t i = 0; i < locality_hosts.size(); ++i) {
    syncLocalityState(per_locality[i], *host_set, i, recreate_child);
  }
}

Upstream::LoadBalancer*
WorkerLocalLb::pickLocalityLb(const std::vector<PerLocalityState>& per_locality,
                              PriorityRoutingWeights::SelectionSource source, size_t preferred_idx,
                              size_t& actual_idx) const {
  auto* lb = per_locality[preferred_idx].stateFor(source).lb.get();
  if (lb != nullptr) {
    actual_idx = preferred_idx;
    return lb;
  }
  // Stale routing snapshot — the preferred locality's child LB was torn down after a host
  // change but before the routing weights were recomputed. Scan for any locality with a
  // usable LB.
  for (size_t i = 0; i < per_locality.size(); ++i) {
    lb = per_locality[i].stateFor(source).lb.get();
    if (lb != nullptr) {
      actual_idx = i;
      return lb;
    }
  }
  return nullptr;
}

WorkerLocalLb::PrioritySourcePick
WorkerLocalLb::resolvePrioritySource(Upstream::LoadBalancerContext* context) {
  using SelectionSource = PriorityRoutingWeights::SelectionSource;
  if (priority_set_.hostSetsPerPriority().empty()) {
    return {0, SelectionSource::Healthy, /*in_panic=*/false, /*fail=*/true};
  }

  const absl::optional<uint64_t> hash_key = context ? context->computeHashKey() : absl::nullopt;
  const uint64_t hash = hash_key.has_value() ? hash_key.value() : random_.random();

  // Live priority/health selection from worker-local LoadBalancerBase state.
  const auto host_set_and_availability = chooseHostSet(context, hash);
  const uint32_t priority = host_set_and_availability.first.priority();

  if (isInPanic(priority)) {
    return {priority, SelectionSource::AllHosts, /*in_panic=*/true,
            /*fail=*/factory_.failTrafficOnPanic()};
  }
  const SelectionSource source =
      host_set_and_availability.second == Upstream::LoadBalancerBase::HostAvailability::Healthy
          ? SelectionSource::Healthy
          : SelectionSource::Degraded;
  return {priority, source, /*in_panic=*/false, /*fail=*/false};
}

Upstream::HostSelectionResponse WorkerLocalLb::chooseHost(Upstream::LoadBalancerContext* context) {
  const auto pick = resolvePrioritySource(context);
  // Panic stat is counted here only; peekAnotherHost deliberately does not double-count it.
  if (pick.in_panic) {
    stats_.lb_healthy_panic_.inc();
  }
  if (pick.fail) {
    return {nullptr};
  }
  const uint32_t priority = pick.priority;
  const PriorityRoutingWeights::SelectionSource source = pick.source;

  auto& per_locality = per_priority_locality_[priority].localities;
  if (per_locality.empty()) {
    return {nullptr};
  }

  // Fetch the routing snapshot once and reuse it for locality selection and the zone-routing stat
  // below (avoids a second TLS lookup per request).
  const auto* snapshot = factory_.routingWeights();
  const size_t preferred_idx = chooseLocality(snapshot, priority, source);
  size_t locality_idx = preferred_idx;
  auto* lb = pickLocalityLb(per_locality, source, preferred_idx, locality_idx);
  if (lb == nullptr) {
    return {nullptr};
  }

  // Endpoint retry is owned by the child LB; delegate with the raw context and return its full
  // response. Record the zone-routing stat once for the selected locality.
  if (snapshot != nullptr && priority < snapshot->priority_weights.size()) {
    const auto& priority_snapshot = snapshot->priority_weights[priority];
    if (priority_snapshot.has_local_locality) {
      if (locality_idx == 0) {
        if (priority_snapshot.allLocalFor(source)) {
          stats_.lb_zone_routing_all_directly_.inc();
        } else {
          stats_.lb_zone_routing_sampled_.inc();
        }
      } else {
        stats_.lb_zone_routing_cross_zone_.inc();
      }
    }
  }

  return lb->chooseHost(context);
}

size_t WorkerLocalLb::chooseLocality(const RoutingWeightsSnapshot* snapshot, uint32_t priority,
                                     PriorityRoutingWeights::SelectionSource source) const {
  const auto& per_locality = per_priority_locality_[priority].localities;
  if (per_locality.size() <= 1) {
    return 0;
  }
  if (snapshot == nullptr || priority >= snapshot->priority_weights.size()) {
    return 0;
  }
  const auto& weights = snapshot->priority_weights[priority].weightsFor(source);
  if (weights.empty()) {
    return 0;
  }

  // Look up each LIVE locality's advisory weight by identity. A locality present on the worker but
  // missing from the snapshot (e.g. just-added, snapshot lagging by ≤1 tick) gets weight 0.0 and is
  // excluded from selection until the next recompute — matching the prior count-mismatch behavior.
  const auto weight_at = [&weights, &per_locality](size_t i) -> double {
    auto it = weights.find(per_locality[i].locality);
    return it != weights.end() ? it->second : 0.0;
  };

  double effective_total = 0.0;
  for (size_t i = 0; i < per_locality.size(); ++i) {
    effective_total += weight_at(i);
  }
  if (effective_total <= 0.0) {
    return 0;
  }

  auto& rng = factory_.random();
  double target = (rng.random() / static_cast<double>(rng.max())) * effective_total;
  double cumulative = 0.0;
  for (size_t i = 0; i < per_locality.size(); ++i) {
    cumulative += weight_at(i);
    if (target < cumulative) {
      return i;
    }
  }

  // Floating-point rounding guard: return the last locality, not an arbitrary one.
  return per_locality.size() - 1;
}

Upstream::HostConstSharedPtr
WorkerLocalLb::peekAnotherHost(Upstream::LoadBalancerContext* context) {
  const auto pick = resolvePrioritySource(context);
  if (pick.fail) {
    return nullptr;
  }
  const uint32_t priority = pick.priority;
  const PriorityRoutingWeights::SelectionSource source = pick.source;

  if (priority >= per_priority_locality_.size()) {
    return nullptr;
  }
  auto& per_locality = per_priority_locality_[priority].localities;
  if (per_locality.empty()) {
    return nullptr;
  }

  const size_t preferred_idx = chooseLocality(factory_.routingWeights(), priority, source);
  size_t actual_idx = preferred_idx;
  auto* lb = pickLocalityLb(per_locality, source, preferred_idx, actual_idx);
  return lb != nullptr ? lb->peekAnotherHost(context) : nullptr;
}

OptRef<Envoy::Http::ConnectionPool::ConnectionLifetimeCallbacks>
WorkerLocalLb::lifetimeCallbacks() {
  return {};
}

absl::optional<Upstream::SelectedPoolAndConnection>
WorkerLocalLb::selectExistingConnection(Upstream::LoadBalancerContext*, const Upstream::Host&,
                                        std::vector<uint8_t>&) {
  return absl::nullopt;
}

} // namespace LoadAwareLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
