Added :ref:`oob_reporting_config
<envoy_v3_api_field_extensions.load_balancing_policies.load_aware_locality.v3.LoadAwareLocality.oob_reporting_config>`
to the ``load_aware_locality`` load balancing policy. It supplies optional
overrides for the ORCA out-of-band reporting connection: an alternative port (e.g. a
reporting sidecar), the ``:authority`` header, and transport socket selection via
``transport_socket_match_criteria``. Honored only when ``enable_oob_load_report`` is true.
This field is API only; the ``load_aware_locality`` load balancer is not yet implemented.
