# PAF fixtures

Inputs for the `read_paf` tests. The reads are cut from a reference at known coordinates
and their names say where each one must align, so a test can derive the expected PAF row
from how the read was built instead of copying it from minimap2's output.

## Files

| File | Contents |
|---|---|
| `k12_ref.fa.gz` | One record, `k12_100000_120000`: E. coli K-12 W3110 (`data/sylph/tiny_refs/e.coli-K12.fasta.gz`) bases `[100000, 120000)`, 0-based half-open |
| `k12_reads.fa.gz` | Seven reads built from that reference; see below |
| `k12_minimap2.paf` | minimap2's PAF for the reads against the reference |
| `k12_minimap2.paf.gz` | The same bytes, gzip-compressed with `gzip -n` |

## Reads

Coordinates are 0-based half-open on `k12_100000_120000`, the same convention PAF uses; `read_paf` reports each interval with both ends `+ 1`.

| Read | Construction | Alignment it must produce |
|---|---|---|
| `fwd_1000_3000` | reference `[1000, 3000)` | `+`, query `[0, 2000)`, target `[1000, 3000)` |
| `fwd_5000_9000` | reference `[5000, 9000)` | `+`, query `[0, 4000)`, target `[5000, 9000)` |
| `rev_10000_13000` | reverse complement of `[10000, 13000)` | `-`, query `[0, 3000)`, target `[10000, 13000)` |
| `rev_14000_18000` | reverse complement of `[14000, 18000)` | `-`, query `[0, 4000)`, target `[14000, 18000)` |
| `ovhfwd_16000_20000` | `[16000, 20000)` then 1500 bp of genome `[3500000, 3501500)` | `+`, query `[0, 4000)` of 5500, target `[16000, 20000)` |
| `ovhrev_17000_20000` | reverse complement of (`[17000, 20000)` then genome `[3510000, 3511500)`) | `-`, query `[1500, 4500)` of 4500, target `[17000, 20000)` |
| `unmapped` | genome `[3600000, 3603000)`, outside the reference | no hit |

The two overhang reads are what make a partial alignment visible. On the reverse one, PAF's
forward-strand query coordinates put the aligned block at the read's 3' end. A reader that
mirrored the coordinates would report `[0, 3000)` instead. Both overhang slices end at the
reference's end, so chance matches cannot extend an alignment into the foreign tail.

minimap2 writes the unmapped read (`--paf-no-hit`) with its query length (3000), strand `*`,
target `*`, and zeros for every other coordinate, length and count. Its only tag is `rl:i:0`.

## Regenerating

Generated with the command-line minimap2 built from the `ext/minimap2` submodule (2.30-r1287,
the-miint fork), the same source `align_minimap2` embeds, as
`minimap2 -x map-hifi -c --paf-no-hit -t 1`. The build is made in a copy of the source,
because `make` writes its objects and binary into the source tree. From the repository root:

```bash
cp -r ext/minimap2 ../minimap2_cli && make -C ../minimap2_cli minimap2
../minimap2_cli/minimap2 -x map-hifi -c --paf-no-hit -t 1 data/paf/k12_ref.fa.gz data/paf/k12_reads.fa.gz \
    > data/paf/k12_minimap2.paf
gzip -n -c data/paf/k12_minimap2.paf > data/paf/k12_minimap2.paf.gz
git diff --stat data/paf/   # expect no diff
```

`align_minimap2` cannot produce this fixture. It returns rows that miint builds from
minimap2's alignment results and never writes PAF. A fixture made from it would need a PAF
writer in miint, and the tests would then check miint's reader against miint's own writer.
`read_paf` has to be tested on what minimap2's own PAF writer emits, and the command-line
program from the same source is that writer. Because both come from one source, the test
that compares `read_paf` with `align_minimap2` on these reads compares one minimap2 version
reached two ways.
