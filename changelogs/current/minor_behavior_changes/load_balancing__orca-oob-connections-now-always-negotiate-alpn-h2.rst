ORCA out-of-band reporting connections now always negotiate ALPN ``h2``, since out-of-band
reporting is always gRPC over HTTP/2. Previously, the connection inherited the cluster
transport socket's ALPN, which could mismatch when the main host used HTTP/1.1.
