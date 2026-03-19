# Design: Moving HostLbPolicyData Out of the Host Abstraction

## Context

PR #43995 converted `HostLbPolicyData` from a single-slot to a vector-based
storage model on the `Host` object. This unblocks PR #43784 (load-aware locality
LB) where multiple LB policies need per-host state simultaneously.

Reviewer @adisuissa noted that the **more correct approach** is to move
`HostLbPolicyData` out of the host abstraction entirely and into the LB policy
implementation. This document proposes a design for that refactoring.

## Problem Statement

The current design has several issues:

1. **Host coupling**: `HostDescription` knows about LB policy data — a concern
   that belongs to the LB layer, not the host layer.
2. **ORCA routing through Host**: The router delivers ORCA reports by iterating
   `host->lbPolicyDataAt(i)`. This couples the router to LB internals.
3. **dynamic_cast lookup**: `typedLbPolicyData<T>()` uses `dynamic_cast` in a
   linear scan — functional but not ideal for a hot path.
4. **Thread-safety burden on Host**: The Host stores mutable LB data that gets
   written from worker threads, yet the Host is documented as "read-only after
   initialization."
5. **Unbounded vector growth**: Nothing prevents multiple `addLbPolicyData()`
   calls from the same policy type, since there's no keying mechanism.

## Proposed Design: LB-Owned Per-Host Data Map

### Core Idea

Each **LB policy instance** owns a side-table mapping `HostConstSharedPtr → PolicyData`.
The Host itself has no knowledge of LB policy data. ORCA reports are delivered
via an **observer/callback** mechanism rather than by poking into the Host.

### Components

#### 1. `LbHostDataMap<T>` — Per-Policy Host Data Store

A template class owned by each LB policy instance that maps hosts to
policy-specific data:

```cpp
// source/common/upstream/lb_host_data_map.h

template <typename T>
class LbHostDataMap {
public:
  // Attach data to a host. Called on main thread during init / host addition.
  void setData(const HostConstSharedPtr& host, std::unique_ptr<T> data) {
    absl::WriterMutexLock lock(&mu_);
    data_[host.get()] = std::move(data);
  }

  // Look up data for a host. Called on worker threads during LB decisions.
  // Returns raw pointer (non-owning). Null if not found.
  T* getData(const Host& host) const {
    absl::ReaderMutexLock lock(&mu_);
    auto it = data_.find(&host);
    return it != data_.end() ? it->second.get() : nullptr;
  }

  // Remove data when a host is removed. Called on main thread.
  void removeData(const Host& host) {
    absl::WriterMutexLock lock(&mu_);
    data_.erase(&host);
  }

private:
  mutable absl::Mutex mu_;
  absl::flat_hash_map<const Host*, std::unique_ptr<T>> data_ ABSL_GUARDED_BY(mu_);
};
```

**Why a map keyed by raw pointer?**
- Hosts are long-lived objects managed by the `PrioritySet`. Their lifetime
  exceeds that of any single request.
- LB policies already hold references to the `PrioritySet` and receive
  host-addition/removal callbacks — they can clean up the map in the removal
  callback.
- No additional ref-counting overhead on the hot path.

**Thread safety:**
- Writes (add/remove) happen on the main thread during cluster membership changes.
- Reads happen on worker threads during `chooseHost()` and ORCA callbacks.
- A reader-writer mutex is appropriate since reads vastly outnumber writes.
- Alternative: use a `ThreadLocalObject` with periodic snapshots from main
  thread, matching the existing `applyWeightsToAllWorkers()` pattern. This would
  eliminate locking on the read path entirely.

#### 2. ORCA Report Delivery — Observer Pattern

Replace the current "iterate host's LB data vector" approach with a
**callback registration** model.

```cpp
// envoy/upstream/orca_observer.h

class OrcaObserver {
public:
  virtual ~OrcaObserver() = default;

  // Called on worker thread when an ORCA report arrives for this host.
  // Implementation must be thread-safe.
  virtual absl::Status onOrcaLoadReport(const HostConstSharedPtr& host,
                                        const OrcaLoadReport& report,
                                        const StreamInfo::StreamInfo& stream_info) PURE;
};

using OrcaObserverSharedPtr = std::shared_ptr<OrcaObserver>;
```

**Registration point**: On the `ClusterInfo` or a new `OrcaDispatcher` object
associated with the cluster:

```cpp
class OrcaDispatcher {
public:
  // Register an observer. Returns a handle for unregistration.
  OrcaObserverHandle addObserver(OrcaObserverSharedPtr observer);

  // Called by the router when an ORCA report is received.
  void dispatchReport(const HostConstSharedPtr& host,
                      const OrcaLoadReport& report,
                      const StreamInfo::StreamInfo& stream_info);
};
```

**Router changes**: Instead of:
```cpp
for (size_t i = 0; i < host->lbPolicyDataCount(); ++i) {
  auto data = host->lbPolicyDataAt(i);
  if (data.has_value() && data->receivesOrcaLoadReport()) {
    data->onOrcaLoadReport(report, stream_info);
  }
}
```

It becomes:
```cpp
cluster.orcaDispatcher().dispatchReport(host, report, stream_info);
```

**Benefits:**
- Router no longer needs to know about LB policy data at all.
- LB policies that don't care about ORCA simply don't register.
- No need for the `receivesOrcaLoadReport()` virtual method.

#### 3. LB Policy Integration

Each LB policy that needs per-host data would:

1. Own a `LbHostDataMap<MyPolicyData>` member.
2. Populate it in `initialize()` and via `PrioritySet` update callbacks.
3. Register as an `OrcaObserver` on the cluster's `OrcaDispatcher`.
4. Look up its own data in the map during `chooseHost()` and ORCA callbacks.

**Example — Client-Side Weighted Round Robin:**

```cpp
class ClientSideWeightedRoundRobinLoadBalancer : public OrcaObserver {
  LbHostDataMap<ClientSideHostLbPolicyData> host_data_;

  absl::Status initialize() override {
    // Populate data for existing hosts
    for (const auto& host : priority_set_.hostSetsPerPriority()) {
      for (const auto& h : host->hosts()) {
        host_data_.setData(h, std::make_unique<ClientSideHostLbPolicyData>(...));
      }
    }
    // Register for host changes
    priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t, const HostVector& added, const HostVector& removed) {
        for (const auto& h : added)
          host_data_.setData(h, std::make_unique<ClientSideHostLbPolicyData>(...));
        for (const auto& h : removed)
          host_data_.removeData(*h);
      });
    // Register for ORCA
    orca_handle_ = cluster_.orcaDispatcher().addObserver(shared_from_this());
    return absl::OkStatus();
  }

  // OrcaObserver
  absl::Status onOrcaLoadReport(const HostConstSharedPtr& host,
                                const OrcaLoadReport& report,
                                const StreamInfo::StreamInfo& si) override {
    if (auto* data = host_data_.getData(*host); data != nullptr) {
      return data->processReport(report, si);
    }
    return absl::OkStatus();
  }
};
```

### Migration Plan

This refactoring can be done incrementally:

#### Phase 1: Add `OrcaDispatcher` (Non-Breaking)
- Introduce `OrcaDispatcher` on `ClusterInfo`.
- Have the router dispatch to both the old Host-based path and the new
  `OrcaDispatcher` path.
- No LB policies change yet.

#### Phase 2: Migrate LB Policies One at a Time
- Migrate CSWR to use `LbHostDataMap` + `OrcaObserver`.
- Migrate Peak EWMA to use `LbHostDataMap` (no ORCA needed).
- Migrate load-aware locality (PR #43784) to use the new pattern from the start.
- Each migration is a self-contained PR.

#### Phase 3: Remove Old Host Interface
- Remove `addLbPolicyData()`, `lbPolicyDataCount()`, `lbPolicyDataAt()`,
  `typedLbPolicyData<T>()` from `HostDescription`.
- Remove `receivesOrcaLoadReport()` and `onOrcaLoadReport()` from
  `HostLbPolicyData` base class.
- Remove the vector storage from `HostDescriptionImplBase`.
- Remove the old Host-based ORCA iteration from the router.
- Delete the `HostLbPolicyData` base class entirely if no longer needed,
  or keep it as a marker interface if useful.

### Impact on PR #43995 (Current Vector Approach)

The current vector-based approach in PR #43995 is **forward-compatible** with
this design:
- The vector storage is an implementation detail inside `HostDescriptionImplBase`
  that gets deleted in Phase 3.
- The `typedLbPolicyData<T>()` template is used by LB policies to find their
  data — these call sites get replaced with `host_data_.getData()` in Phase 2.
- No additional complexity is introduced by landing #43995 first.

**Conclusion**: Landing #43995 as-is is safe. It doesn't make the eventual
transition harder — the code it adds is exactly the code that gets removed in
Phase 3, with no tendrils into other subsystems.

### Alternatives Considered

#### A. Thread-Local Slot on Host
Store a `SlotAllocator`-based slot on the Host, similar to `StreamInfo`
filter state. Rejected because it still couples the Host to LB concerns and
adds TLS machinery complexity.

#### B. `absl::any` Map on Host
Replace the typed vector with `absl::flat_hash_map<std::string, absl::any>`.
Rejected because it still keeps data on the Host and adds string-keyed lookup
overhead.

#### C. Keep Vector, Add ORCA Observer Only
Keep per-host data on the Host but move ORCA delivery to the observer pattern.
This is a partial solution — it addresses the router coupling but not the
Host abstraction concern. Could be a valid Phase 1.5 stopping point if full
migration is deprioritized.

## Open Questions

1. **Where does `OrcaDispatcher` live?** On `ClusterInfo` (most natural) or as
   a standalone object? `ClusterInfo` is already large.
2. **Lifetime management for `OrcaObserver`**: Should the handle be RAII
   (destructor unregisters) or explicit? RAII is safer.
3. **Performance**: Is the reader-writer mutex acceptable, or should we use the
   TLS snapshot pattern from the start? The TLS pattern is more complex but
   zero-cost on worker threads.
4. **LRS path**: The router also dispatches ORCA to Load Reporting Service.
   Should `OrcaDispatcher` handle both LRS and LB, or remain LB-only?
