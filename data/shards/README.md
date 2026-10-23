# Minimap2 index fixtures

## Multi-part index: `multipart_fixture.mmi`

`align_minimap2_multipart.test` streams this index part by part, and `rype_filter_multipart.test` checks that `rype_filter` refuses it. It is two single-part indexes, `multipart_fixture_part1.mmi` and `multipart_fixture_part2.mmi`, concatenated.

`save_minimap2_index` always builds one part. The minimap2 CLI's `-I <batch>` builds several by writing one `mm_idx_dump` block per part into the same file, and each single-part file is already a complete block, so concatenating two gives the same layout without the CLI:

```sql
CREATE TABLE p1 AS SELECT * FROM (VALUES
    ('part1_ref', 'ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTGGCCTTAAGGCCTTAAGGCCTTAAGGCCTTAAGGCCTTAAGGCCTTAAGGCC')
) AS t(read_id, sequence1);
CREATE TABLE p2 AS SELECT * FROM (VALUES
    ('part2_ref', 'TTTTGGGGCCCCAAAATTTTGGGGCCCCAAAATTTTGGGGCCCCAAAATTTTAAAACCCCGGGGTTTTAAAACCCCGGGGTTTTAAAACCCCGGGGTTTTAAAA')
) AS t(read_id, sequence1);
SELECT success FROM save_minimap2_index('p1', 'data/shards/multipart_fixture_part1.mmi', k := 5);
SELECT success FROM save_minimap2_index('p2', 'data/shards/multipart_fixture_part2.mmi', k := 5);
```

```bash
cat data/shards/multipart_fixture_part1.mmi data/shards/multipart_fixture_part2.mmi > data/shards/multipart_fixture.mmi
```

The two single-part files are kept beside the multi-part one, so that `align_minimap2_multipart.test` can compare the multi-part run against two single-index runs.

The test's query sequences are prefixes of the two references above. Change a reference without changing the queries and the test does not fail loudly: the read stops matching and its row disappears, which is what most of its assertions look for. Edit the two together.
