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

The purpose of zone aware routing is to send as much traffic to the local zone in the upstream
cluster as possible while roughly maintaining the same number of requests per second across all
upstream hosts (depending on load balancing policy).

.. _arch_overview_load_balancing_zone_aware_routing_preconditions:

Preconditions
^^^^^^^^^^^^^

Zone aware routing requires all of the following conditions to be met. If any check fails, Envoy
falls back to normal (non-locality-aware) load balancing.

1. Neither the originating nor the upstream cluster is in
   :ref:`panic mode <arch_overview_load_balancing_panic_threshold>`.
2. Zone aware :ref:`routing is enabled <config_cluster_manager_cluster_runtime_zone_routing>`
   (the ``routing_enabled`` percentage check passes).
3. The originating cluster has hosts in the local locality (i.e., the local Envoy's zone is
   represented in the originating cluster).
4. The upstream cluster has hosts in at least 2 localities.
5. The originating cluster has hosts in at least 2 localities. Note: when
   :ref:`force_local_zone <envoy_v3_api_msg_extensions.load_balancing_policies.common.v3.LocalityLbConfig.ZoneAwareLbConfig.ForceLocalZone>`
   is enabled, this requirement on the originating cluster is relaxed.
6. The upstream cluster has at least ``min_cluster_size`` healthy hosts (default: 6).

Routing states
^^^^^^^^^^^^^^

Envoy uses a three-state model to determine how to route traffic. The state is recomputed whenever
cluster membership or health changes.

**NoLocalityRouting**
  One or more preconditions listed above are not met. Envoy routes without locality preference,
  distributing traffic across all healthy hosts in the upstream cluster.

**LocalityDirect**
  The upstream cluster's local zone has enough capacity to absorb all local traffic. This occurs
  when:

  * The local zone's share of the upstream cluster is greater than or equal to its share of the
    originating cluster, *or*
  * :ref:`force_local_zone <envoy_v3_api_msg_extensions.load_balancing_policies.common.v3.LocalityLbConfig.ZoneAwareLbConfig.ForceLocalZone>`
    is enabled and at least ``min_size`` healthy upstream hosts exist in the local zone.

  In this state, 100% of traffic from the local zone is routed to the local zone of the upstream
  cluster.

**LocalityResidual**
  The local zone in the upstream cluster cannot absorb all traffic from the local zone of the
  originating cluster. This occurs when the originating cluster's local zone percentage is greater
  than the upstream cluster's local zone percentage. In this state, Envoy splits traffic:

  * A portion is routed directly to the local zone.
  * The remainder (residual) is distributed across other zones proportional to their residual
    capacity.

.. _arch_overview_load_balancing_zone_aware_routing_residual:

Residual capacity algorithm
^^^^^^^^^^^^^^^^^^^^^^^^^^^

When in the **LocalityResidual** state, Envoy calculates traffic splits as follows:

1. **Per-locality percentages** are computed for both the originating and upstream clusters. Each
   locality's percentage is its share of healthy hosts (or host weight, if ``locality_basis`` is set
   to ``HEALTHY_HOSTS_WEIGHT``). Percentages are internally scaled by 10,000 for precision.

2. **Local zone direct percentage**: The fraction of local traffic that can be sent to the local
   upstream zone is:

   .. code-block:: text

     local_percent_to_route = upstream_local_pct / originating_local_pct

   For example, if the originating cluster has 40% of hosts in ``us-east-1a`` and the upstream has
   20%, then 50% of local traffic goes directly to the local zone.

3. **Residual capacity**: For each non-local zone, residual capacity is computed as:

   .. code-block:: text

     residual[i] = max(0, upstream_pct[i] - originating_pct[i])

   Zones where the upstream percentage exceeds the originating percentage have spare capacity to
   accept cross-zone traffic. Traffic is distributed across these zones proportionally to their
   residual capacity using a cumulative distribution and random sampling.

**Worked example**: Consider 3 zones with originating percentages (40%, 40%, 20%) and upstream
percentages (25%, 50%, 25%):

* Local zone (zone 0): ``local_percent_to_route = 25% / 40% = 62.5%``
* Zone 1 residual: ``50% - 40% = 10%`` (spare capacity)
* Zone 2 residual: ``25% - 20% = 5%`` (spare capacity)
* Cross-zone traffic is distributed 2:1 between zone 1 and zone 2.

Overprovisioning interaction
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The health percentages used by zone aware routing are affected by the
:ref:`overprovisioning factor <arch_overview_load_balancing_overprovisioning_factor>`. With the
default factor of 1.4, a locality is not considered degraded until its healthy host percentage drops
below approximately 72% (since ``100% / 1.4 ≈ 72%``). This means small fluctuations in host health
within a zone do not immediately alter zone-aware routing decisions.

Zone count mismatch
^^^^^^^^^^^^^^^^^^^

The originating and upstream clusters do not need to have the same number of zones. Envoy supports
zone-aware routing across clusters with different zone configurations. Residual capacity is
calculated per-locality using the matching algorithm described above, regardless of whether
the zone sets are identical.

.. _arch_overview_load_balancing_zone_aware_routing_lb_policies:

Supported load balancer policies
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Not all load balancer policies support zone-aware routing. The table below summarizes support:

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Load Balancer Policy
     - Zone-Aware Routing
     - Locality-Weighted LB
   * - Round Robin
     - Yes
     - Yes
   * - Least Request
     - Yes
     - Yes
   * - Random
     - Yes
     - Yes
   * - Ring Hash
     - No
     - Yes
   * - Maglev
     - No
     - Yes

Ring Hash and Maglev use consistent hashing, which requires deterministic host selection. Zone-aware
routing's dynamic locality switching is incompatible with this requirement. These policies support
:ref:`locality-weighted load balancing <arch_overview_load_balancing_locality_weighted_lb>` instead,
where the management server provides explicit locality weights via EDS.

.. _arch_overview_load_balancing_zone_aware_routing_configuration:

Configuration
^^^^^^^^^^^^^

Zone aware routing can be configured in two ways: the legacy ``CommonLbConfig`` approach and the
modern extension-based ``LocalityLbConfig`` approach.

Legacy configuration (CommonLbConfig)
"""""""""""""""""""""""""""""""""""""

The legacy approach uses
:ref:`zone_aware_lb_config <envoy_v3_api_msg_config.cluster.v3.Cluster.CommonLbConfig.ZoneAwareLbConfig>`
in the cluster's ``common_lb_config`` block. This exposes three fields:

* ``routing_enabled``: Percentage of requests that will be routed to the local zone (default: 100%).
* ``min_cluster_size``: Minimum number of healthy upstream hosts required to enable zone-aware
  routing (default: 6).
* ``fail_traffic_on_panic``: If true, fail all requests when the cluster is in panic mode instead
  of distributing to all hosts (default: false).

.. code-block:: yaml

  clusters:
  - name: my_cluster
    type: EDS
    eds_cluster_config:
      eds_config:
        api_config_source:
          api_type: GRPC
          grpc_services:
          - envoy_grpc:
              cluster_name: xds_cluster
    common_lb_config:
      zone_aware_lb_config:
        routing_enabled:
          value: 100
        min_cluster_size: 6
        fail_traffic_on_panic: false

Modern extension-based configuration (LocalityLbConfig)
"""""""""""""""""""""""""""""""""""""""""""""""""""""""

The modern approach uses
:ref:`LocalityLbConfig.ZoneAwareLbConfig <envoy_v3_api_msg_extensions.load_balancing_policies.common.v3.LocalityLbConfig.ZoneAwareLbConfig>`
inside the ``load_balancing_policy`` block. This provides the same three fields as the legacy API
plus three additional fields:

* ``routing_enabled``, ``min_cluster_size``, ``fail_traffic_on_panic``: Same as legacy.
* ``force_local_zone``: A
  :ref:`ForceLocalZone <envoy_v3_api_msg_extensions.load_balancing_policies.common.v3.LocalityLbConfig.ZoneAwareLbConfig.ForceLocalZone>`
  message that forces all traffic to the local zone when at least ``min_size`` (default: 1) healthy
  upstream hosts exist there. This overrides the normal proportional distribution algorithm and also
  relaxes the precondition requiring the originating cluster to have hosts in at least 2 localities.
* ``locality_basis``: A
  :ref:`LocalityBasis <envoy_v3_api_enum_extensions.load_balancing_policies.common.v3.LocalityLbConfig.ZoneAwareLbConfig.LocalityBasis>`
  enum controlling how per-locality percentages are computed:

  * ``HEALTHY_HOSTS_NUM`` (default): Use the number of healthy hosts per locality.
  * ``HEALTHY_HOSTS_WEIGHT``: Use the sum of healthy host weights per locality. Use this when
    hosts have heterogeneous weights.
* ``force_locality_direct_routing``: **Deprecated** — replaced by ``force_local_zone``.

.. code-block:: yaml

  clusters:
  - name: my_cluster
    type: EDS
    eds_cluster_config:
      eds_config:
        api_config_source:
          api_type: GRPC
          grpc_services:
          - envoy_grpc:
              cluster_name: xds_cluster
    load_balancing_policy:
      policies:
      - typed_extension_config:
          name: envoy.load_balancing_policies.round_robin
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.load_balancing_policies.round_robin.v3.RoundRobin
            locality_lb_config:
              zone_aware_lb_config:
                routing_enabled:
                  value: 100
                min_cluster_size: 6
                fail_traffic_on_panic: false
                force_local_zone:
                  min_size: 3

Priority limitation
^^^^^^^^^^^^^^^^^^^

When using multiple priorities, zone aware routing is currently only supported for P=0.
