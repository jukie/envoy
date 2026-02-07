.. _arch_overview_load_balancing_zone_aware_routing:

Zone aware routing
------------------

We use the following terminology:

* **Originating/Upstream cluster**: Envoy routes requests from an originating cluster to an upstream
  cluster.
* **Local zone**: The same zone that contains a subset of hosts in both the originating and
  upstream clusters.
* **Zone aware routing**: Best effort routing of requests to an upstream cluster host in the local
  zone.

In deployments where hosts in originating and upstream clusters belong to different zones
Envoy performs zone aware routing. There are several preconditions before zone aware routing can be
performed:

.. _arch_overview_load_balancing_zone_aware_routing_preconditions:

* Both originating and upstream cluster are not in
  :ref:`panic mode <arch_overview_load_balancing_panic_threshold>`.
* Zone aware :ref:`routing is enabled <config_cluster_manager_cluster_runtime_zone_routing>`.
* The upstream cluster has enough hosts. See
  :ref:`here <config_cluster_manager_cluster_runtime_zone_routing>` for more information.

The purpose of zone aware routing is to send as much traffic to the local zone in the upstream
cluster as possible while roughly maintaining the same number of requests per second across all
upstream hosts (depending on load balancing policy).

Envoy tries to push as much traffic as possible to the local upstream zone as long as
roughly the same number of requests per host in the upstream cluster are maintained. The decision of
whether Envoy routes to the local zone or performs cross zone routing depends on the percentage of
healthy hosts in the originating cluster and upstream cluster in the local zone. There are two cases
with regard to percentage relations in the local zone between originating and upstream clusters:

* The originating cluster local zone percentage is greater than the one in the upstream cluster.
  In this case we cannot route all requests from the local zone of the originating cluster to the
  local zone of the upstream cluster because that will lead to request imbalance across all upstream
  hosts. Instead, Envoy calculates the percentage of requests that can be routed directly to the
  local zone of the upstream cluster. The rest of the requests are routed cross zone. The specific
  zone is selected based on the residual capacity of the zone (that zone will get some local zone
  traffic and may have additional capacity Envoy can use for cross zone traffic).
* The originating cluster local zone percentage is smaller than the one in upstream cluster.
  In this case the local zone of the upstream cluster can get all of the requests from the
  local zone of the originating cluster and also have some space to allow traffic from other zones
  in the originating cluster (if needed).

Note that when using multiple priorities, zone aware routing is currently only supported for P=0.

.. _arch_overview_load_balancing_zone_aware_routing_locality_basis:

Locality basis
^^^^^^^^^^^^^^

By default, zone aware routing computes per-zone percentages using the number of healthy hosts
in each zone (``HEALTHY_HOSTS_NUM``). The
:ref:`locality_basis <envoy_v3_api_field_extensions.load_balancing_policies.common.v3.LocalityLbConfig.ZoneAwareLbConfig.locality_basis>`
field on
:ref:`ZoneAwareLbConfig <envoy_v3_api_msg_extensions.load_balancing_policies.common.v3.LocalityLbConfig.ZoneAwareLbConfig>`
controls how these percentages are computed. Three modes are available:

* ``HEALTHY_HOSTS_NUM`` (default): Percentages are proportional to the count of healthy hosts
  in each zone. A zone with 3 out of 10 total hosts gets 30%.
* ``HEALTHY_HOSTS_WEIGHT``: Percentages are proportional to the sum of host weights in each
  zone. Useful when hosts have heterogeneous capacity.
* ``LRS_REPORTED_RATE``: Percentages use control-plane-provided traffic fractions instead of
  host counts. See :ref:`LRS-reported rate <arch_overview_load_balancing_zone_aware_routing_lrs_reported_rate>`
  below.

.. _arch_overview_load_balancing_zone_aware_routing_lrs_reported_rate:

LRS-reported rate locality basis
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``LRS_REPORTED_RATE`` mode addresses a limitation of the default host-count-based approach.
Zone aware routing normally assumes that traffic is distributed proportionally across zones based on
the number of Envoy instances in each zone: a zone with 30% of instances is assumed to handle 30%
of inbound traffic. This assumption breaks under BGP anycast skew or heterogeneous client
distributions, where some zones receive disproportionately more traffic than their instance count
suggests.

In ``LRS_REPORTED_RATE`` mode, the control plane aggregates
:ref:`Load Reporting Service (LRS) <arch_overview_load_reporting_service>` data from all Envoy
instances to compute the actual per-zone traffic fractions. These fractions are pushed back to
Envoy via the
:ref:`observed_traffic_fraction <envoy_v3_api_field_config.endpoint.v3.LocalityLbEndpoints.observed_traffic_fraction>`
field in the local cluster's EDS response. Zone aware routing then uses these observed fractions
for the originating cluster percentages instead of computing them from host counts.

Only the originating (local) cluster percentages change. The upstream cluster percentages continue
to use host counts or weights, as they represent available capacity rather than traffic demand.
This preserves the existing zone aware routing semantics: local preference, spillover to zones
with residual capacity, and all other existing behavior.

**Data flow:**

::

  Envoy instances ──► LRS reports ──► Control plane aggregates
  Control plane computes per-zone traffic fractions
  Control plane sets observed_traffic_fraction in local cluster EDS
  Envoy reads fractions during zone-aware percentage computation

**Fallback behavior:**

If fractions are not available (e.g., on first startup) or become stale (exceed
the configured
:ref:`staleness_threshold <envoy_v3_api_field_extensions.load_balancing_policies.common.v3.LocalityLbConfig.ZoneAwareLbConfig.LrsRateConfig.staleness_threshold>`),
the load balancer automatically falls back to ``HEALTHY_HOSTS_NUM`` behavior. This ensures safe
degradation if the control plane stops providing updated fractions.

**Example:**

Consider a deployment with 3 zones where BGP skew causes zone-a to receive 50% of traffic
despite having only 30% of Envoy instances:

::

  Without LRS_REPORTED_RATE:
    local_percentage:    zone-a=30%  zone-b=50%  zone-c=20%  (from host counts)
    upstream_percentage: zone-a=30%  zone-b=50%  zone-c=20%
    Result: zone-a routes 100% locally (30% <= 30%), overloading zone-a upstreams

  With LRS_REPORTED_RATE:
    local_percentage:    zone-a=50%  zone-b=35%  zone-c=15%  (from observed fractions)
    upstream_percentage: zone-a=30%  zone-b=50%  zone-c=20%  (from host counts)
    Result: zone-a routes 60% locally, spills 40% to zones with excess capacity

See :ref:`How do I configure LRS-reported rate zone aware routing? <common_configuration_lrs_reported_rate_zone_aware_routing>`
for configuration details.

