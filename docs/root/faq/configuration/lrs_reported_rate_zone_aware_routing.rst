.. _common_configuration_lrs_reported_rate_zone_aware_routing:

How do I configure LRS-reported rate zone aware routing?
========================================================

:ref:`LRS-reported rate zone aware routing <arch_overview_load_balancing_zone_aware_routing_lrs_reported_rate>`
uses control-plane-provided traffic fractions to correct for BGP skew or uneven client distribution
across zones. This page describes how to configure it.

Prerequisites
-------------

All :ref:`zone aware routing prerequisites <common_configuration_zone_aware_routing>` apply. In addition:

* The control plane must aggregate LRS data and compute per-zone traffic fractions.
* The control plane must set
  :ref:`observed_traffic_fraction <envoy_v3_api_field_config.endpoint.v3.LocalityLbEndpoints.observed_traffic_fraction>`
  on the **local cluster's** EDS response.
* LRS must be enabled so the control plane receives traffic data from Envoy instances.

Envoy configuration
-------------------

Set ``locality_basis`` to ``LRS_REPORTED_RATE`` in the zone aware load balancing config. This is
configured on the load balancing policy used by the upstream cluster (e.g., ``round_robin``).

.. code-block:: yaml

  static_resources:
    clusters:
    - name: cluster_b
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
                  locality_basis: LRS_REPORTED_RATE
                  lrs_rate_config:
                    staleness_threshold: 120s
                    fraction_source: EDS_FIELD

  cluster_manager:
    local_cluster_name: cluster_a

The ``staleness_threshold`` controls how long Envoy uses previously received fractions before
falling back to host-count-based percentages. Set this to 2-3x the expected EDS push interval.
Valid range: 5s to 600s.

The ``fraction_source`` determines where Envoy reads traffic fractions from:

* ``EDS_FIELD`` (default, recommended): reads from the
  :ref:`observed_traffic_fraction <envoy_v3_api_field_config.endpoint.v3.LocalityLbEndpoints.observed_traffic_fraction>`
  field in
  :ref:`LocalityLbEndpoints <envoy_v3_api_msg_config.endpoint.v3.LocalityLbEndpoints>`.
* ``LOCALITY_METADATA``: reads from locality metadata under the key
  ``envoy.lb.observed_traffic_fraction``. This is a transitional mechanism for control planes
  that cannot yet produce the new EDS proto field.

Local cluster EDS response
--------------------------

The control plane must set ``observed_traffic_fraction`` on the local cluster's EDS response.
Values are in basis points (0-10000, where 10000 = 100%). For example, if zone-a handles 50%
of total inbound traffic and zone-b handles 35%:

.. code-block:: yaml

  cluster_name: cluster_a
  endpoints:
  - locality:
      zone: us-east-1a
    lb_endpoints:
    - endpoint:
        address:
          socket_address:
            address: 10.0.0.1
            port_value: 8080
    observed_traffic_fraction:
      value: 5000
  - locality:
      zone: us-east-1b
    lb_endpoints:
    - endpoint:
        address:
          socket_address:
            address: 10.0.1.1
            port_value: 8080
    observed_traffic_fraction:
      value: 3500
  - locality:
      zone: us-east-1c
    lb_endpoints:
    - endpoint:
        address:
          socket_address:
            address: 10.0.2.1
            port_value: 8080
    observed_traffic_fraction:
      value: 1500

Control plane requirements
--------------------------

The control plane needs to:

1. **Collect LRS reports** from all Envoy instances. Each report contains per-locality request
   counts via ``upstream_locality_stats`` in
   :ref:`ClusterStats <envoy_v3_api_msg_config.endpoint.v3.ClusterStats>`.

2. **Aggregate and compute fractions.** Sum request rates across all reporting instances per
   locality, then compute each locality's share as a fraction of the total. Use EWMA or a
   sliding window to smooth transient spikes.

3. **Inject fractions into EDS.** Set ``observed_traffic_fraction`` on the local cluster's
   ``LocalityLbEndpoints`` in each EDS push. The same fractions can be sent to all Envoy
   instances (global view).

4. **Handle bootstrap.** On initial startup with no LRS data, either omit the field (Envoy
   falls back to host counts) or set fractions proportional to host counts as an initial
   estimate.

Verification steps
------------------

* Use :ref:`per zone <config_cluster_manager_cluster_per_az_stats>` Envoy stats to monitor
  cross zone traffic and verify that spillover occurs when expected.
* Compare the ``upstream_rq_zone`` stats before and after enabling ``LRS_REPORTED_RATE`` to
  verify that traffic distribution changes under skew conditions.
* If fractions become stale, Envoy logs a warning and falls back to host-count-based routing.
  Monitor for staleness warnings in the Envoy access log.

Comparison with other locality modes
-------------------------------------

+-----------------------+----------------------------+-------------------------------+------------------------------+
| Aspect                | ``HEALTHY_HOSTS_NUM``      | ``HEALTHY_HOSTS_WEIGHT``      | ``LRS_REPORTED_RATE``        |
+=======================+============================+===============================+==============================+
| Data source           | Host count                 | Host weight                   | Control plane (LRS)          |
+-----------------------+----------------------------+-------------------------------+------------------------------+
| Accuracy under skew   | Assumes proportional       | Assumes proportional          | Uses observed fractions      |
+-----------------------+----------------------------+-------------------------------+------------------------------+
| Control plane changes | None                       | None                          | LRS aggregation + EDS        |
+-----------------------+----------------------------+-------------------------------+------------------------------+
| Fallback              | N/A (default)              | N/A                           | Falls back to host counts    |
+-----------------------+----------------------------+-------------------------------+------------------------------+
| Best for              | Uniform traffic            | Heterogeneous host capacity   | BGP skew, uneven clients     |
+-----------------------+----------------------------+-------------------------------+------------------------------+
