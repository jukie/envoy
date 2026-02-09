# Research: Load-Aware Zone Routing via LRS + locality_basis Mode

**Issue:** en-bd3
**Date:** 2026-02-09
**Status:** Complete

## Problem Statement

Zone-aware routing uses host counts (or host weights) to calculate routing
percentages between localities. When inbound traffic isn't evenly distributed
across zones, this leads to unequal load distribution among upstream hosts.

**Example:** If Zone A has 3 upstream hosts and Zone B has 3 upstream hosts,
zone-aware routing assigns 50%/50%. But if Zone A receives 80% of inbound
traffic while Zone B receives 20%, Zone A's hosts are overloaded while Zone
B's hosts are underutilized.

**Constraint:** No ORCA support yet. Need a solution that gives each Envoy
instance visibility into per-zone load.

## How LRS Currently Works

### Protocol

LRS uses a **bidirectional gRPC stream** (`StreamLoadStats`) defined in
`api/envoy/service/load_stats/v3/lrs.proto`:

1. Envoy opens a persistent stream to the management server
2. Management server sends `LoadStatsResponse` specifying which clusters to
   report and the `load_reporting_interval`
3. Envoy periodically sends `LoadStatsRequest` containing aggregated load stats
4. Management server uses reports to compute global traffic assignments

### Data Collected (per locality)

From `api/envoy/config/endpoint/v3/load_report.proto`, `UpstreamLocalityStats`:

| Field | Description |
|-------|-------------|
| `locality` | Region, zone, sub_zone |
| `total_successful_requests` | Completed successfully |
| `total_error_requests` | Failed (5xx/gRPC errors) |
| `total_requests_in_progress` | Currently in-flight |
| `total_issued_requests` | Total issued in interval |
| `total_active_connections` | Active connections (WIP) |
| `cpu_utilization` | From ORCA |
| `mem_utilization` | From ORCA |
| `application_utilization` | From ORCA |
| `load_metric_stats` | Custom named metrics |

### Implementation

- **Client:** `source/common/upstream/load_stats_reporter.{h,cc}`
- **Owner:** `ClusterManagerImpl` creates and owns `LoadStatsReporter`
- **Config:** `cluster_manager.load_stats_config` in bootstrap proto
- **Data source:** Host stats (`rq_success_`, `rq_error_`, `rq_total_`,
  `rq_active_`) are latched per reporting interval

### Critical Architectural Detail

LRS is a **one-way reporting channel**: Envoy -> management server. Each Envoy
reports its OWN observed upstream load. The LRS proto documentation explicitly
states the intended closed-loop flow (lrs.proto lines 56-57):

> "The management server uses the load reports from all reported Envoys from
> around the world, computes global assignment and prepares traffic assignment
> destined for each zone Envoys are located in."

The management server closes the loop by pushing updated assignments via EDS.

## How Zone-Aware Routing Currently Works

### Configuration

From `api/envoy/extensions/load_balancing_policies/common/v3/common.proto`:

```protobuf
message ZoneAwareLbConfig {
  enum LocalityBasis {
    HEALTHY_HOSTS_NUM = 0;    // Count of healthy hosts (default)
    HEALTHY_HOSTS_WEIGHT = 1; // Sum of host weights
  }

  Percent routing_enabled = 1;           // Default: 100%
  UInt64Value min_cluster_size = 2;      // Default: 6
  bool fail_traffic_on_panic = 3;
  ForceLocalZone force_local_zone = 5;
  LocalityBasis locality_basis = 6;
}
```

### Core Algorithm

In `source/extensions/load_balancing_policies/common/load_balancer_impl.cc`,
`calculateLocalityPercentages()` (line 653):

1. For each locality in local and upstream host sets, compute a weight based
   on `locality_basis_`:
   - `HEALTHY_HOSTS_NUM`: weight = number of healthy hosts
   - `HEALTHY_HOSTS_WEIGHT`: weight = sum of `host->weight()` for healthy hosts
2. Calculate percentages: `10000 * locality_weight / total_weight`
3. These percentages drive routing state:
   - **LocalityDirect:** If upstream % >= local %, all traffic goes local
   - **LocalityResidual:** Local fills first, excess distributed by residual
     capacity across other localities

### Integration Point

The `switch (locality_basis_)` at lines 664 and 688 is the exact extension
point where a new basis mode would be added. The existing code has a
`default: PANIC_DUE_TO_CORRUPT_ENUM` case that enforces exhaustive handling.

## Architecture Analysis: How Would Envoy Consume Aggregated LRS Data?

The proposal asks: "how would an Envoy instance consume aggregated LRS data?"
There are three possible architectures:

### Option A: Management Server Aggregates + EDS Locality Weights (No Envoy Changes)

**Flow:**
```
All Envoys --LRS--> Management Server --aggregates--> Computes optimal weights
Management Server --EDS update--> locality_weights per locality
Envoys use locality_weighted_lb_config with updated weights
```

**Pros:**
- Already works today with no Envoy code changes
- Management server has global view of all traffic
- Uses existing `locality_weighted_lb_config` path
- Well-tested, production-proven mechanism

**Cons:**
- Requires management server implementation (outside Envoy)
- EDS update latency (seconds to minutes)
- Switches from zone-aware to locality-weighted LB (different algorithm)
- Cannot use zone-aware routing's local-preference behavior

### Option B: New xDS Extension Carrying Aggregated Load Data

**Flow:**
```
All Envoys --LRS--> Management Server --aggregates-->
Management Server --new xDS resource--> Per-zone load data
Envoy consumes in zone-aware routing as locality_basis
```

**Pros:**
- Envoy gets global load visibility
- Can integrate directly with zone-aware routing algorithm
- Clean separation of concerns

**Cons:**
- Requires new xDS resource type (significant API work)
- Needs xDS protocol extension (UDPA review process)
- Complex: new proto, new client, new data path
- Overkill when EDS weights can achieve the same goal

### Option C: Envoy Uses Own Locally-Observed Load Stats

**Flow:**
```
Envoy observes its own per-zone traffic (same stats LRS reports)
Envoy uses these local observations as locality_basis
No management server round-trip needed
```

**Pros:**
- No management server changes needed
- Low latency (uses real-time local observations)
- Simple implementation: new enum value + switch case
- Natural extension of existing `locality_basis` mechanism

**Cons:**
- Each Envoy only sees its OWN traffic, not global traffic
- In heterogeneous deployments, different Envoys may make different decisions
- Only helps when a single Envoy's view correlates with global load
- Doesn't account for requests from OTHER Envoys to the same upstream zone

## What Changes to Zone-Aware Routing Would Be Needed

For **Option C** (the most scoped Envoy-side change):

### Proto Change

```protobuf
enum LocalityBasis {
  HEALTHY_HOSTS_NUM = 0;
  HEALTHY_HOSTS_WEIGHT = 1;
  OBSERVED_LOAD = 2;  // New: use locally-observed request rates
}
```

### C++ Changes

In `load_balancer_impl.cc`, add a case to both switch statements in
`calculateLocalityPercentages()`:

```cpp
case LocalityLbConfig::ZoneAwareLbConfig::OBSERVED_LOAD:
  for (const auto& host : locality_hosts) {
    // Use observed request rate as weight
    // Could use rq_total (throughput) or rq_active (concurrency)
    locality_weight += host->stats().rq_total_.value();
  }
  break;
```

The `ZoneAwareLoadBalancerBase` class would need to:
1. Accept the new enum value (already handled by proto)
2. Potentially track request rates over a sliding window (the raw counters are
   cumulative, not per-interval)
3. Handle the cold-start case where no traffic has been observed yet (fall back
   to HEALTHY_HOSTS_NUM)

### Additional Considerations

- **Sliding window:** Raw `rq_total` is cumulative. Need either:
  - A periodic snapshot mechanism (like LRS's latch)
  - Or use `rq_active` (current in-flight) which is a gauge, not a counter
- **Update frequency:** `calculateLocalityPercentages` is called on host set
  changes, not periodically. A load-based approach might need periodic
  recalculation triggered by a timer.
- **Stats thread safety:** Host stats are atomic counters, safe to read from
  the LB thread.

## Feasibility Assessment

### Is This a Reasonable Approach?

**Yes, with caveats.** The approach is viable but the right solution depends on
the deployment model:

| Scenario | Best Approach |
|----------|--------------|
| Have a sophisticated management server (e.g., Istio, custom xDS) | **Option A**: Server-side aggregation + EDS locality weights. No Envoy changes. |
| Want Envoy to self-adjust without management server changes | **Option C**: Add `OBSERVED_LOAD` locality_basis. Limited but useful. |
| Need global load visibility in Envoy | **Option B**: New xDS extension. High effort, questionable ROI. |

### Fundamental Blockers

**No fundamental blockers.** The existing architecture supports this:

1. The `locality_basis` extension point already exists and was recently added
   (PR #39803, commit `a9c654ec39`)
2. The host stats needed are already collected and are thread-safe
3. The LRS infrastructure already aggregates the same data we'd use
4. The management server feedback loop via EDS is already designed for this

### Risks and Limitations

1. **Option C's local-only view:** An Envoy in Zone A only sees traffic it
   sends, not traffic sent by other Envoys. If the goal is to balance load
   across ALL upstream hosts globally, local observations may be insufficient.

2. **Feedback loops:** If all Envoys in a zone simultaneously shift traffic
   away from an overloaded zone, the target zone becomes overloaded. Damping
   or hysteresis would be needed.

3. **Cold start:** Before traffic flows, there's no load data. Need graceful
   fallback to host-count-based routing.

4. **Counter semantics:** Using cumulative counters vs. rates requires
   careful design. The LRS reporter's `latch()` mechanism shows a proven
   pattern for snapshotting.

## Prior Art

### In the Envoy Codebase

- **`locality_basis` field** added in PR #39803 (`a9c654ec39`): Introduced
  `HEALTHY_HOSTS_WEIGHT` as alternative to host counts. This is the direct
  precedent for adding another basis mode.
- **`force_local_zone`** added in PR #39058: Shows zone-aware routing is
  actively evolving.
- **`WrrLocality` LB policy** (PR #40577, #41689): Enables locality-weighted
  round-robin, the alternative path for management-server-controlled weights.
- **Client-Side Weighted Round Robin** with locality support: Already uses
  EDS locality weights for load distribution.

### In the xDS Ecosystem

- **LRS protocol** explicitly describes the management-server-aggregation
  feedback loop (Option A). This is the canonical intended use.
- **ORCA** (Open Request Cost Aggregation): Provides per-request cost metrics
  from upstream, but is out of scope per the constraint.

### Related GitHub Activity

- PR #39803 is the most relevant precedent (adding `HEALTHY_HOSTS_WEIGHT`)
- The LRS tests in `test/integration/load_stats_integration_test.cc` (1015
  lines) show comprehensive coverage of the reporting path
- No existing PRs or issues propose using LRS data directly for routing
  decisions within Envoy (the intended path is management server aggregation)

## Recommendation

**For most deployments: Option A (management server aggregation via EDS).**
This is how LRS was designed to be used, requires no Envoy changes, and
provides the global view needed for accurate load-based routing.

**If Envoy-side changes are desired: Option C (OBSERVED_LOAD locality_basis).**
This is a well-scoped, incremental change that extends the existing
`locality_basis` mechanism. It's most useful when:
- Each Envoy instance handles a representative slice of traffic
- The management server is simple and can't compute optimal weights
- Low-latency adaptation is more important than global optimality

**Option B (new xDS extension) is not recommended** due to the complexity
and the availability of simpler alternatives.

## Files Referenced

| File | Purpose |
|------|---------|
| `api/envoy/service/load_stats/v3/lrs.proto` | LRS gRPC service definition |
| `api/envoy/config/endpoint/v3/load_report.proto` | Load report message types |
| `api/envoy/extensions/load_balancing_policies/common/v3/common.proto` | ZoneAwareLbConfig, LocalityBasis enum |
| `source/common/upstream/load_stats_reporter.{h,cc}` | LRS client implementation |
| `source/extensions/load_balancing_policies/common/load_balancer_impl.{h,cc}` | Zone-aware routing core logic |
| `source/extensions/load_balancing_policies/common/locality_wrr.{h,cc}` | Locality-weighted round robin |
| `test/integration/load_stats_integration_test.cc` | LRS integration tests |
| `test/extensions/load_balancing_policies/round_robin/round_robin_lb_test.cc` | Zone-aware routing tests |
