# ENA run fixtures

Files that `test/scripts/ena_webin_mock.py` serves in place of ENA's file servers, so that
`read_ena_sequences` can be tested, and the onboarding tutorials run, without EBI.

| File | Source | md5 | Bytes | Reads |
|---|---|---|---|---|
| `ERR1074767.fastq.gz` | `https://ftp.sra.ebi.ac.uk/vol1/fastq/ERR107/007/ERR1074767/ERR1074767.fastq.gz`, downloaded 2026-10-22 | `2b0eb247ca19c4bc29fa4f2399786a6f` | 1376946 | 20939 |

ERR1074767 is a single-end American Gut Project run (study PRJEB11419, sample SAMEA3610311,
experiment ERX1154891). The md5 and size match the `fastq_md5` and `fastq_bytes` ENA's Portal API
reported for it that day. The mock computes both from the file, and takes the run's other metadata
from its `_READ_RUNS` table, which holds ENA's values for each file here.
