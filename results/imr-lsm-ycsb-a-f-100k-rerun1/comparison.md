# YCSB Workload A-F comparison

All values are medians of successful runs. Latency is in microseconds.

| Workload | Run throughput (ops/s) | Read op | Read avg | Read P99 | Write op | Write avg | IMR WA | Metadata WA |
|---|---:|---|---:|---:|---|---:|---:|---:|
| workloada | 11344.299489506522 | READ | 5.055953946057926 | 12 | UPDATE | 10.31623759990361 | 1.000000 | 4.687435 |
| workloadb | 11851.149561507465 | READ | 4.942005049442457 | 11 | UPDATE | 12.60852407261247 | 1.000000 | 4.778971 |
| workloadc | 13285.505513484788 | READ | 4.62355 | 11 | NONE | NA | 1.000000 | 4.829230 |
| workloadd | 13182.17769575534 | READ | 2.8622179599321314 | 9 | INSERT | 8.121698297789083 | 1.000000 | 4.777353 |
| workloade | 6652.032195835828 | SCAN | 77.80699962084509 | 170 | INSERT | 12.068574836016696 | 1.000000 | 4.778473 |
| workloadf | 11532.695190866105 | READ | 4.87979 | 12 | READ-MODIFY-WRITE | 11.637023811911973 | 1.000000 | 4.769546 |

For Workload E, the read columns report SCAN latency. Workloads D/E use INSERT as the write operation, and Workload F uses READ-MODIFY-WRITE. Workload C has no write operation, so its write-latency fields are `NA`. A WA field is `NA` only when that phase produces no corresponding writes.
