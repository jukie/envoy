# Research: Alternative Approaches to Load-Aware Zone Routing (Non-ORCA)

**Issue:** en-b90
**Date:** 2026-02-09
**Author:** polecat/nux

## Problem Statement

Zone-aware routing in Envoy uses host counts (or host weights) to calculate routing
percentages across localities. When inbound traffic distribution is uneven across zones,
this leads to unequal upstream host loading. We need per-zone load visibility to make
smarter routing decisions, **without requiring ORCA support on upstream services**.

## Current State of Zone-Aware Routing in Envoy

### How It Works Today

Envoy's zone-aware routing is implemented in `ZoneAwareLoadBalancerBase`
(`source/extensions/load_balancing_policies/common/load_balancer_impl.h`). It operates
in three states:

1. **NoLocalityRouting** - Zone-aware routing disabled (fallback)
2. **LocalityDirect** - All queries route to local locality (when local zone can handle 100%)
3. **LocalityResidual** - Local locality handles its share, residual distributed to other zones

The routing decision is based on a **host count ratio**: what percentage of total healthy
hosts are in each zone. This is a static proxy for load capacity and doesn't reflect
actual resource utilization.

**Configuration** (`CommonLbConfig` in `cluster.proto`):
```
locality_config_specifier: {
  zone_aware_lb_config: {
    routing_enabled: <percent>     # default 100%
    min_cluster_size: <uint64>     # default 6
    fail_traffic_on_panic: <bool>
  }
}
```

### Locality-Weighted Load Balancing (Existing Alternative)

Envoy already supports **locality-weighted LB** as a mutually exclusive alternative to
zone-aware routing (`locality_weighted_lb_config` in `CommonLbConfig`). Key details:

- Weights come from EDS via `LocalityLbEndpoints.load_balancing_weight`
- Implemented in `LocalityWrr` (`source/extensions/load_balancing_policies/common/locality_wrr.h`)
- Uses EDF (Earliest Deadline First) scheduling for proportional weighted selection
- Effective weight formula: `weight * min(1.0, (overprovisioning_factor / 100) * availability_ratio)`
- The overprovisioning factor defaults to 140%

**Key insight:** `locality_weighted_lb_config` + dynamic EDS weight updates is the most
direct path to load-aware zone routing without ORCA.

---

## Approach 1: Dynamic EDS Locality Weights via External Feedback Loop

### How It Works

A management server (or control plane component) aggregates load metrics from external
monitoring systems (Prometheus, etc.) and dynamically adjusts `LocalityLbEndpoints.load_balancing_weight`
values in EDS responses.

**Architecture:**
```
[Prometheus/Monitoring] → [Weight Calculator] → [xDS Management Server] → [EDS Push] → [Envoy]
```

The weight calculator:
1. Collects per-zone metrics (CPU, memory, request latency, error rates) from monitoring
2. Computes relative zone capacities
3. Translates capacities into EDS locality weights
4. Pushes updated `ClusterLoadAssignment` via EDS

### Data Sources for Weight Calculation

Envoy already reports per-locality stats via **Load Reporting Service (LRS)**
(`api/envoy/config/endpoint/v3/load_report.proto`):

- `total_successful_requests` per locality
- `total_error_requests` per locality
- `total_requests_in_progress` per locality
- `total_issued_requests` per locality
- `total_active_connections` per locality
- `total_new_connections` per locality
- `total_fail_connections` per locality

These are available **without ORCA** and provide substantial signal about zone load.

### Assessment

| Criterion | Rating |
|-----------|--------|
| **Implementation complexity** | **Low-Medium** (Control plane changes only; no Envoy code changes) |
| **Data freshness** | **Medium** (Limited by LRS reporting interval + weight calculation + EDS push latency; typically 10-60s feedback loop) |
| **Accuracy of load signal** | **Good** (Can incorporate multiple signals: request rates, error rates, latency percentiles, infrastructure metrics) |
| **Operational complexity** | **Medium** (Requires weight calculation logic, tuning of feedback loop parameters, monitoring of the controller itself) |

### Strengths
- Zero Envoy code changes; config-only on the Envoy side
- Leverages existing EDS and LRS protocols
- Weight calculation can be arbitrarily sophisticated (incorporate infra metrics, cost, etc.)
- Well-understood pattern (traffic engineering via control plane)

### Weaknesses
- Feedback loop latency (not suitable for sub-second load spikes)
- Requires building/maintaining weight calculation service
- Risk of oscillation if feedback loop is poorly tuned
- All Envoy instances get the same weights (no per-instance view)

---

## Approach 2: Adaptive Routing via Outlier Detection / Circuit Breaker Stats

### How It Works

Use Envoy's existing per-host outlier detection and circuit breaker statistics as
implicit load signals. Hosts under heavy load exhibit higher latency and error rates,
which outlier detection already tracks.

**Available per-host stats** (`envoy/upstream/host_description.h`):
- `rq_success`, `rq_error`, `rq_timeout`, `rq_total` (request counters)
- `cx_connect_fail`, `cx_total` (connection counters)
- `cx_active`, `rq_active` (gauges)

**Outlier detection** (`envoy/upstream/outlier_detection.h`):
- `successRate()` per host (external and local origin)
- Consecutive error tracking
- Host ejection status

### Mechanism

This approach doesn't directly adjust zone weights but uses two complementary mechanisms:

1. **Outlier ejection reduces effective zone capacity**: When hosts in an overloaded zone
   get ejected, the locality's healthy host count drops, and zone-aware routing automatically
   sends less traffic there (since it uses healthy host counts).

2. **Priority spillover**: If a zone's health drops below the panic threshold, traffic
   spills to other priorities/zones.

**Enhancement path**: A custom LB policy could aggregate per-host stats within each
locality to compute a dynamic "zone health score" and adjust selection probabilities.

### Assessment

| Criterion | Rating |
|-----------|--------|
| **Implementation complexity** | **None to Low** (Built-in behavior) / **Medium** (for custom LB policy) |
| **Data freshness** | **Good** (Per-request granularity for outlier detection; sub-second) |
| **Accuracy of load signal** | **Poor-Medium** (Only reacts after degradation is observable; lagging indicator) |
| **Operational complexity** | **Low** (Outlier detection already deployed) / **Medium** (custom LB policy) |

### Strengths
- Already built into Envoy; zero additional infrastructure
- Sub-second reaction time for ejection-based load shedding
- Battle-tested in production

### Weaknesses
- Reactive, not proactive (load must cause failures before routing adjusts)
- Coarse-grained (ejection is binary; no gradual weight reduction)
- Outlier detection is designed for individual bad hosts, not zone-level overload
- No direct mechanism to rebalance traffic before errors occur

---

## Approach 3: Custom Load Balancing Policy Extension

### How It Works

Implement a custom `TypedLoadBalancerFactory` that extends zone-aware routing with
load-aware logic. The extension point is well-defined:

```cpp
// envoy/upstream/load_balancer.h
class TypedLoadBalancerFactory : public Config::TypedFactory {
  virtual ThreadAwareLoadBalancerPtr create(...) = 0;
};
```

A custom policy could:
1. Maintain per-locality running averages of latency and error rate
2. Use `HostLbPolicyData` to store per-host load estimates
3. Override locality selection to prefer less-loaded zones
4. Use EDF scheduling with dynamically computed zone weights

### Available Hooks

- `HostLbPolicyData`: Per-host data storage (line 98, `host_description.h`)
- `LoadBalancerContext`: Per-request context including downstream headers, connection info
- `HostSet`: Per-locality host lists, weights, health status
- `PrioritySet::MemberUpdateCb`: Notification of host set changes

### Assessment

| Criterion | Rating |
|-----------|--------|
| **Implementation complexity** | **High** (Full Envoy C++ extension; requires deep understanding of LB internals) |
| **Data freshness** | **Excellent** (Can update on every request completion) |
| **Accuracy of load signal** | **Good** (Access to all per-host metrics in real-time) |
| **Operational complexity** | **High** (Custom code to maintain, test, deploy; tied to Envoy release cycle) |

### Strengths
- Maximum flexibility and data freshness
- Can implement any algorithm (P2C with zone awareness, weighted least-connections per zone, etc.)
- Integrated into Envoy's hot path with minimal overhead

### Weaknesses
- Significant engineering investment
- Must handle all edge cases (zone changes, host membership churn, thread safety)
- Tied to Envoy internals; may break across versions
- Each Envoy instance has only its own view of load (no global visibility)

---

## Approach 4: Locality-Weighted LB with Subset Load Balancing

### How It Works

Combine `locality_weighted_lb_config` with subset load balancing. The subset LB has
two relevant features:

1. **`locality_weight_aware`**: When enabled, subset selection considers locality weights
2. **`scale_locality_weight`**: Scales locality weight by the ratio of hosts in the subset
   vs. total hosts, evening out load when subsets are unevenly distributed across zones

From `cluster.proto`:
```proto
message LbSubsetConfig {
  bool locality_weight_aware = 4;
  bool scale_locality_weight = 5;
}
```

### Assessment

| Criterion | Rating |
|-----------|--------|
| **Implementation complexity** | **None** (Config-only; existing Envoy features) |
| **Data freshness** | **N/A** (Static configuration) |
| **Accuracy of load signal** | **Poor** (Doesn't actually measure load; corrects for subset distribution) |
| **Operational complexity** | **Low** |

### Strengths
- Already available; config-only change
- Useful specifically when subset predicates cause uneven zone distribution

### Weaknesses
- Only addresses subset-related zone imbalance, not general load imbalance
- Still uses host counts as proxy for capacity; not truly load-aware

---

## Approach 5: LRS + Control Plane Feedback Loop (Detailed Design)

### How It Works

This is a more detailed version of Approach 1, specifically designed around LRS data.

**Architecture:**
```
[Envoy Fleet] --LRS--> [LRS Server] --metrics--> [Weight Controller] --EDS--> [Envoy Fleet]
                                                        ↑
                                                   [Prometheus/External metrics]
```

**LRS provides these locality-level metrics without ORCA:**
- Request success/error/in-progress counts per locality
- Connection counts per locality
- All aggregated from per-host `rq_*` and `cx_*` counters

**Weight calculation algorithm:**
```
For each locality L:
  success_rate_L = total_successful_requests_L / total_issued_requests_L
  inflight_ratio_L = total_requests_in_progress_L / host_count_L
  error_rate_L = total_error_requests_L / total_issued_requests_L

  # Compute a load score (lower is better)
  load_score_L = inflight_ratio_L * (1 + error_penalty * error_rate_L)

  # Invert to get capacity weight (higher is better)
  capacity_weight_L = 1.0 / max(load_score_L, epsilon)

# Normalize weights and push via EDS
total = sum(capacity_weight_L for all L)
For each locality L:
  eds_weight_L = round(capacity_weight_L / total * 10000)
```

### Assessment

| Criterion | Rating |
|-----------|--------|
| **Implementation complexity** | **Medium** (Build LRS consumer + weight calculator + EDS publisher) |
| **Data freshness** | **Medium** (LRS interval + processing time; ~10-30s typical) |
| **Accuracy of load signal** | **Good** (Uses actual request metrics, not just host counts) |
| **Operational complexity** | **Medium** (New service to operate, but well-bounded responsibility) |

### Key Design Considerations
- **Dampening**: Apply exponential moving average to avoid weight oscillation
- **Min weight floor**: Never set a locality weight to 0; always maintain minimum traffic for health checking
- **Ramp rate**: Limit how fast weights can change per interval
- **Fallback**: If LRS data is stale, fall back to equal weights

---

## Approach 6: Ring Hash / Maglev with Zone Awareness

### How It Works

Ring hash and Maglev LB policies provide consistent hashing. Zone awareness can be
layered on top:

1. Configure ring hash/Maglev as the LB policy
2. Use `zone_aware_lb_config` or `locality_weighted_lb_config`
3. The consistent hash determines host selection within the chosen locality

Both support `LocalityLbConfig` (from `common.proto`):
```proto
message LocalityLbConfig {
  oneof locality_config_specifier {
    ZoneAwareLbConfig zone_aware_lb_config = 1;
    LocalityWeightedLbConfig locality_weighted_lb_config = 2;
  }
}
```

### Assessment

| Criterion | Rating |
|-----------|--------|
| **Implementation complexity** | **None** (Config-only) |
| **Data freshness** | **N/A** (Static hash ring) |
| **Accuracy of load signal** | **Poor** (Hash-based; doesn't account for load) |
| **Operational complexity** | **Low** |

### Strengths
- Useful when consistent hashing is needed (caching, session affinity)
- Zone preference still applies

### Weaknesses
- Not load-aware; consistent hashing distributes based on key, not load
- Zone awareness is just first-tier selection; within zone, it's hash-based

---

## Relevant Envoy Issues and Proposals

1. **Issue #6333**: [M:N zone-aware routing](https://github.com/envoyproxy/envoy/issues/6333) -
   Discusses zone-aware routing when local and upstream zone counts differ.

2. **Issue #28419**: [Zone-aware LB incorrectly handles mismatched localities](https://github.com/envoyproxy/envoy/issues/28419) -
   Bug in zone-aware LB when local/upstream locality sets don't match in sorted order.

3. **Issue #25692**: [Envoy not using locality weights within same priority](https://github.com/envoyproxy/envoy/issues/25692) -
   Reports locality weights being ignored in certain configurations.

4. **Issue #37867**: [Zone-aware routing: cluster state source](https://github.com/envoyproxy/envoy/issues/37867) -
   Questions about where zone-aware routing gets its cluster state.

5. **`client_side_weighted_round_robin`**: The existing ORCA-based LB policy
   (`source/extensions/load_balancing_policies/client_side_weighted_round_robin/`) demonstrates
   Envoy's extension model for load-aware balancing. A similar non-ORCA approach could reuse
   this pattern with different signal sources.

---

## Recommendation Summary

| Approach | Complexity | Freshness | Accuracy | Best For |
|----------|-----------|-----------|----------|----------|
| **1. Dynamic EDS weights (external)** | Low-Med | ~10-60s | Good | Most deployments; pragmatic choice |
| **2. Outlier detection (built-in)** | None | Sub-second | Poor | Already deployed; defense layer |
| **3. Custom LB policy** | High | Per-request | Good | Large-scale, performance-critical |
| **4. Subset + locality weights** | None | N/A | Poor | Subset-specific imbalance only |
| **5. LRS feedback loop** | Medium | ~10-30s | Good | When you already have LRS infrastructure |
| **6. Ring hash + zone awareness** | None | N/A | Poor | Hash-based workloads only |

### Recommended Path

**Primary: Approach 1/5 (Dynamic EDS Locality Weights via LRS Feedback Loop)**

This is the highest-value approach because:
- **No Envoy code changes required** - pure control plane work
- **Uses existing protocols** (LRS for metrics, EDS for weight delivery)
- **Good accuracy** - real request metrics, not just host counts
- **Proven pattern** - many production deployments use control-plane-driven weight adjustment
- **Incremental** - can start simple (request rate balancing) and add sophistication

**Secondary: Approach 2 (Outlier Detection) as defense-in-depth**

This is already available and provides reactive protection against zone overload.
It's not a replacement for proactive load-aware routing but is a valuable safety net.

**Future: Approach 3 (Custom LB Policy) if tighter feedback loop needed**

If the 10-30s feedback loop of Approach 1/5 proves insufficient, a custom LB policy
provides per-request granularity using locally observed latency and error rates.
This is a significant investment but achievable using the existing `TypedLoadBalancerFactory`
extension point.
