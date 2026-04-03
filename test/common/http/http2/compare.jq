# Pairs nghttp2 (codec=0) and oghttp2 (codec=1) results, computes overhead,
# and formats as an ASCII table with human-readable workload descriptions.

# Map benchmark names to readable descriptions.
def describe:
  if   . == "benchmarkRoundTrip/0/5/32/0"     then [0, "Round-trip, 5 headers, 32B values"]
  elif . == "benchmarkRoundTrip/0/15/64/1024"  then [1, "Round-trip, 15 headers, 64B values, 1K body"]
  elif . == "benchmarkRoundTrip/0/50/128/0"    then [2, "Round-trip, 50 headers, 128B values"]
  elif . == "benchmarkEncodeHeaders/0/5/32"    then [3, "Encode headers, 5 headers, 32B values"]
  elif . == "benchmarkEncodeHeaders/0/15/64"   then [4, "Encode headers, 15 headers, 64B values"]
  elif . == "benchmarkEncodeHeaders/0/50/128"  then [5, "Encode headers, 50 headers, 128B values"]
  elif . == "benchmarkDecodeHeaders/0/5/32"    then [6, "Decode headers, 5 headers, 32B values"]
  elif . == "benchmarkDecodeHeaders/0/15/64"   then [7, "Decode headers, 15 headers, 64B values"]
  elif . == "benchmarkDecodeHeaders/0/50/128"  then [8, "Decode headers, 50 headers, 128B values"]
  elif . == "benchmarkDataTransfer/0/1024"     then [9, "Data transfer, 1K body"]
  elif . == "benchmarkDataTransfer/0/65536"    then [10, "Data transfer, 64K body"]
  else [99, .]
  end;

def fmt_time:
  if . >= 100 then "\(. | round) us"
  elif . >= 10 then "\(. * 10 | round / 10) us"
  else "\(. * 100 | round / 100) us"
  end;

def rpad(n): . + (" " * (n - length));
def lpad(n): (" " * (n - length)) + .;

# Pair up codec=0 and codec=1 results.
[.benchmarks[] | {
  name,
  time: .real_time,
  group: (.name | sub("/[01]/"; "//") | sub("/[01]$"; "/"))
}] |
group_by(.group) |
map(select(length == 2)) |
map((.[0].name | describe) as $d | {
  order: $d[0],
  desc: $d[1],
  nghttp2: (.[0].time | fmt_time),
  oghttp2: (.[1].time | fmt_time),
  overhead: "+\(((.[1].time - .[0].time) / .[0].time * 100) | round)%"
}) |
sort_by(.order) |

# Compute column widths.
(map(.desc | length) | max) as $w0 |
(map(.nghttp2 | length) | max | if . < 7 then 7 else . end) as $w1 |
(map(.oghttp2 | length) | max | if . < 7 then 7 else . end) as $w2 |
(map(.overhead | length) | max | if . < 8 then 8 else . end) as $w3 |

# Draw the table.
def hline(l; m; r):
  l + ("-" * ($w0 + 2)) + m + ("-" * ($w1 + 2)) + m + ("-" * ($w2 + 2)) + m + ("-" * ($w3 + 2)) + r;

hline("┌"; "┬"; "┐"),
"│ \("Workload" | rpad($w0)) │ \("NgHttp2" | lpad($w1)) │ \("OgHttp2" | lpad($w2)) │ \("Overhead" | lpad($w3)) │",
hline("├"; "┼"; "┤"),
(.[] |
  "│ \(.desc | rpad($w0)) │ \(.nghttp2 | lpad($w1)) │ \(.oghttp2 | lpad($w2)) │ \(.overhead | lpad($w3)) │"
),
hline("└"; "┴"; "┘")
