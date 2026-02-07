# API Review Notes: LRS_REPORTED_RATE for Upstream Submission

**Reviewer:** polecat/furiosa
**Date:** 2026-02-06
**Branch:** feature/lrs-reported-rate-zone-routing
**Issue:** en-tvb

## Files Reviewed

1. `api/envoy/extensions/load_balancing_policies/common/v3/common.proto`
   - `LocalityBasis` enum: `LRS_REPORTED_RATE = 2`
   - `LrsRateConfig` message (fields 1-2)
   - `ZoneAwareLbConfig` field `lrs_rate_config = 7`
2. `api/envoy/config/endpoint/v3/endpoint_components.proto`
   - `observed_traffic_fraction = 10` on `LocalityLbEndpoints`
3. `source/extensions/load_balancing_policies/common/load_balancer_impl.cc` (C++ implementation)
4. `source/extensions/load_balancing_policies/common/load_balancer_impl.h` (C++ header)
5. `docs/design/load_aware_zone_routing_v3.md` (design document)

---

## Issues Found

### MUST FIX — Would Block Upstream API Review

#### 1. Missing validation on `observed_traffic_fraction` (endpoint_components.proto:250)

The field is declared as:
```protobuf
google.protobuf.UInt32Value observed_traffic_fraction = 10;
```

The comment states the value is in basis points (0-10000), but there is **no validation rule**.
Other fields in the same message have validation (e.g., `load_balancing_weight` has
`[(validate.rules).uint32 = {gte: 1}]`, `priority` has `[(validate.rules).uint32 = {lte: 128}]`).

**Fix:** Add validation:
```protobuf
google.protobuf.UInt32Value observed_traffic_fraction = 10
    [(validate.rules).uint32 = {lte: 10000}];
```

This prevents misconfigured control planes from pushing values >100% and ensures Envoy rejects
invalid EDS responses at config validation time rather than silently misbehaving.

#### 2. LRS_REPORTED_RATE enum comment references C++ implementation internals (common.proto:42-49)

The comment contains:
```
// - ``local_percentage`` in zone-aware routing uses control-plane fractions
//   instead of host counts.
// - ``upstream_percentage`` continues to use host counts/weights (capacity-based).
```

`local_percentage` and `upstream_percentage` are internal C++ variable names in
`calculateLocalityPercentages()`. Proto API comments should describe **user-facing behavior**,
not implementation variables. Upstream API reviewers will flag this.

**Recommended rewrite:**
```protobuf
// Use traffic distribution fractions provided by the control plane via EDS.
// The control plane aggregates LRS data from all Envoy instances to compute
// per-locality traffic fractions and pushes them back via the
// ``observed_traffic_fraction`` field in ``LocalityLbEndpoints``.
//
// When this mode is active:
//
// - The local cluster's per-locality traffic share is determined by
//   control-plane-provided fractions rather than host counts.
// - Upstream cluster capacity is still computed from host counts/weights.
// - Falls back to ``HEALTHY_HOSTS_NUM`` behavior if fractions are not
//   available or stale.
//
// This preserves zone-aware routing semantics (local preference, spillover)
// while using globally accurate traffic distribution data.
```

#### 3. Proto comment uses markdown bold instead of RST convention (endpoint_components.proto:248)

The comment contains `**local cluster**` which is markdown syntax. Envoy's proto documentation
uses RST-style formatting. Other proto comments in this file use plain text or RST directives
(`:ref:`, `.. attention::`). Upstream reviewers enforce consistent documentation style.

**Fix:** Replace `**local cluster**` with plain text or an RST emphasis directive.

#### 4. `locality_basis` field comment is too terse for API review (common.proto:96-100)

The comment on `locality_basis = 6`:
```
// Determines how locality percentages are computed:
// - HEALTHY_HOSTS_NUM: proportional to the count of healthy hosts.
// - HEALTHY_HOSTS_WEIGHT: proportional to the weights of healthy hosts.
// - LRS_REPORTED_RATE: uses control-plane-provided traffic fractions.
// Default value is HEALTHY_HOSTS_NUM if unset.
```

This duplicates what's in the enum without adding context. Upstream convention is for field
comments to explain the field's role and link to the enum, not repeat enum values.

**Suggested:**
```protobuf
// Controls how per-locality traffic percentages are computed for the local
// cluster in zone-aware routing. See ``LocalityBasis`` enum for options.
// If not set, defaults to ``HEALTHY_HOSTS_NUM``.
LocalityBasis locality_basis = 6;
```

---

### SHOULD FIX — Likely Review Feedback

#### 5. Clarify `observed_traffic_fraction` behavior when value is 0 (endpoint_components.proto:244)

The comment says:
```
// If not set, the load balancer falls back to host-count-based
// percentage computation for this locality.
```

But doesn't address what happens when `observed_traffic_fraction` is explicitly set to 0.
The C++ implementation treats 0 the same as unset (in `updateLrsReportedFractions()`, values
`<= 0` are skipped). This should be documented:

**Fix:** Change to:
```
// If not set or set to 0, the load balancer falls back to host-count-based
// percentage computation for this locality.
```

#### 6. `LRS_REPORTED_RATE_VALUE` constant workaround is stale (load_balancer_impl.cc:33-37)

The C++ code defines:
```cpp
constexpr int LRS_REPORTED_RATE_VALUE = 2;
```
with comment: "Once the proto change merges, this should be replaced..."

The proto change **is already merged** (commit 3a17a108 is on main and this branch).
All usages of `static_cast<int>(locality_basis_) == LRS_REPORTED_RATE_VALUE` should be
replaced with the actual proto enum:
```cpp
locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::LRS_REPORTED_RATE
```

This affects lines: 37, 450, 471, 712, 758 in load_balancer_impl.cc.

#### 7. Implementation only supports LOCALITY_METADATA source, not EDS_FIELD (load_balancer_impl.cc:588-618)

`updateLrsReportedFractions()` reads fractions exclusively from host metadata
(`envoy.lb.observed_traffic_fraction`). However:

- The `FractionSource` enum default is `EDS_FIELD = 0` (the recommended source)
- The proto added an actual `observed_traffic_fraction` field on `LocalityLbEndpoints`
- The `LrsRateConfig.fraction_source` config field is never read in the C++ code

The implementation should:
1. Read `fraction_source` from config
2. When `EDS_FIELD`: read from the proto field on `LocalityLbEndpoints`
3. When `LOCALITY_METADATA`: read from host metadata (current behavior)

Without this, users who set `fraction_source: EDS_FIELD` (or leave the default) will get
no fraction data, silently falling back to host counts.

#### 8. No staleness timer implemented (load_balancer_impl.h:509)

The header comment says: "Timer-based staleness will be added when the full LRS integration
is wired." The proto exposes `staleness_threshold` with validation (5s-600s, default 60s)
but the C++ never reads it or creates a timer. `isLrsFractionsStale()` just returns the
boolean flag.

This means if the control plane stops sending fraction data, stale fractions remain
in use indefinitely — the safety net described in the design doc is not implemented.

For upstream submission, either:
- Implement the timer, or
- Remove `staleness_threshold` from the proto and add it later when the timer is ready.
  An unimplemented config field is worse than a missing one — it gives users false confidence.

---

### CONSIDER — Naming and Design Discussions

#### 9. Enum name `LRS_REPORTED_RATE` vs alternatives

The bead asks us to consider alternatives. Analysis:

| Name | Pros | Cons |
|------|------|------|
| `LRS_REPORTED_RATE` | Describes data origin (LRS reports) | Data arrives via EDS, not LRS; might confuse |
| `CONTROL_PLANE_REPORTED` | Generic, accurate | Vague; doesn't hint at what's reported |
| `OBSERVED_TRAFFIC` | Matches proto field name | Doesn't indicate who observes |
| `XDS_PROVIDED_RATE` | Accurate delivery mechanism | Too generic; many things are xDS-provided |
| `OBSERVED_TRAFFIC_FRACTION` | Matches EDS field exactly | Long |

**Recommendation:** `LRS_REPORTED_RATE` is acceptable. The name correctly identifies the
data source (LRS reports aggregated by control plane). The delivery mechanism (EDS) is
an implementation detail. If upstream reviewers push back, `OBSERVED_TRAFFIC_FRACTION`
would be the strongest alternative since it matches the EDS field name exactly.

#### 10. Scope of `observed_traffic_fraction` — local cluster only vs general

The design doc and comments note this field is "typically set on the local cluster."
The question is whether the field's placement on `LocalityLbEndpoints` (a general EDS message)
is appropriate given its narrow use case.

**Assessment:** The placement is fine. The field is:
- Clearly documented as used by zone-aware routing only
- Ignored when not configured (zero cost)
- On the message where the data naturally lives (per-locality info in EDS)
- Potentially useful for future features beyond zone-aware routing

Restricting it to local cluster only would require a separate proto message, adding
complexity without benefit. Upstream reviewers are unlikely to object.

#### 11. `LrsRateConfig` — nested message vs top-level

`LrsRateConfig` is nested inside `ZoneAwareLbConfig`. This is correct since it's only
used there. However, if future features (e.g., locality-weighted LB) want similar
config, it would need to be moved. This is acceptable technical debt — move it if/when needed.

---

## Upstream Submission Checklist

### Proto Style Compliance

| Check | Status | Notes |
|-------|--------|-------|
| Field numbering correct | PASS | `locality_basis=6`, `lrs_rate_config=7`, `observed_traffic_fraction=10` all use next-free |
| `[#next-free-field]` updated | PASS | `ZoneAwareLbConfig: 8`, `LocalityLbEndpoints: 11` both correct |
| snake_case field names | PASS | All field names follow convention |
| UPPER_SNAKE_CASE enum values | PASS | `LRS_REPORTED_RATE`, `EDS_FIELD`, `LOCALITY_METADATA` |
| Enum zero value is default/unspecified | PASS | `HEALTHY_HOSTS_NUM=0` (existing default), `EDS_FIELD=0` (recommended default) |
| Validation rules present | **FAIL** | Missing on `observed_traffic_fraction` — see issue #1 |
| Comments describe user behavior | **FAIL** | Implementation internals leaked — see issue #2 |
| Documentation format consistent | **FAIL** | Markdown bold in RST context — see issue #3 |
| No breaking changes | PASS | All additions are new fields/messages/enum values |
| Wrapper types used correctly | PASS | `UInt32Value` for optional semantics on `observed_traffic_fraction` |
| Duration type used correctly | PASS | `staleness_threshold` uses `google.protobuf.Duration` with validation |

### Backwards Compatibility

| Check | Status |
|-------|--------|
| New enum value doesn't change default behavior | PASS |
| New fields are optional / have safe defaults | PASS |
| Existing field semantics unchanged | PASS |
| Wire format compatible (field numbers don't conflict) | PASS |

### Implementation Alignment

| Check | Status | Notes |
|-------|--------|-------|
| C++ reads all config fields | **FAIL** | `fraction_source` and `staleness_threshold` not read |
| Default config produces expected behavior | PASS | Falls back to host counts when unconfigured |
| Proto enum used (not magic constants) | **FAIL** | Uses `LRS_REPORTED_RATE_VALUE = 2` constant |
| EDS_FIELD source implemented | **FAIL** | Only LOCALITY_METADATA path works |

---

## Summary

The proto API design is sound. The `LocalityBasis` extension point, EDS delivery via
`observed_traffic_fraction`, and the `LrsRateConfig` configuration are well-thought-out
and fit cleanly into the existing Envoy API surface.

**Before upstream submission, fix:**

1. Add `[(validate.rules).uint32 = {lte: 10000}]` to `observed_traffic_fraction`
2. Rewrite `LRS_REPORTED_RATE` enum comment to remove implementation variable names
3. Fix markdown bold to RST-style in `observed_traffic_fraction` comment
4. Replace `LRS_REPORTED_RATE_VALUE` constant with actual proto enum
5. Implement `EDS_FIELD` fraction source (or document that only `LOCALITY_METADATA` is supported and make it the default)
6. Either implement `staleness_threshold` timer or remove the field from proto until ready

Items 1-3 are proto-only fixes (small). Items 4-6 are C++ implementation gaps that affect
whether the proto API contract is fulfilled. An upstream reviewer will test the API contract
matches the implementation.
