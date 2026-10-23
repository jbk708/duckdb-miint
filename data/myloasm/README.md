# myloasm fixtures

## `ecoli_sim/`

Real myloasm output for simulated HiFi-like reads (`ecoli_sim_reads.fa.gz`). Each read's
name records where it came from, so the myloasm reader tests can check their results against
how the reads were built rather than against myloasm's own description of its output.

Only the files miint reads are kept, at the paths myloasm writes them. The ones myloasm writes
as text are committed gzipped, with `.gz` added to the name. The functions read a directory as
myloasm writes it, so each myloasm test first decompresses these into a test directory.

| File | What it is |
|---|---|
| `assembly_primary.fa.gz` | Polished primary contigs |
| `alternate_assemblies/assembly_alternate.fa.gz` | Contigs myloasm set aside as alternates of a primary; sequences included |
| `alternate_assemblies/duplicated_contigs.fa.gz` | Contigs myloasm set aside as duplicates; sequences included |
| `final_contig_graph.gfa.gz` | Contig graph: `S`, read-placement `a` lines, and `L` links, on the unpolished contigs |
| `3-mapping/map_to_unitigs.paf.gz` | The final mapping of every read to the unpolished contigs; gzipped by myloasm |
| `0-cleaning_and_unitigs/read_coverages.txt.gz` | Round-1 chimera breakpoints; gzipped by myloasm |
| `0-cleaning_and_unitigs/remap_temp/read_coverages.txt.gz` | Round-2 chimera breakpoints (empty here; see below); gzipped by myloasm |
| `myloasm.log.gz` | The run's log, renamed from myloasm's `myloasm_<timestamp>.log`, with the `SYSTEM HOST NAME` value replaced by `redacted` |

### The reads

`ecoli_sim_reads.fa.gz` holds the 1006 reads (11.6 Mb) the output was assembled from, each
cut from E. coli K-12 W3110 (`data/sylph/tiny_refs/e.coli-K12.fasta.gz`). Each name is
`s<index>_<kind>_<fields>_<f|r>`, with coordinates 0-based half-open on that genome. `r`
means the read is the reverse complement of the sequence its fields describe; odd indices
are reversed. A `chim` or `ovh` read is reversed as a whole, so on an `r` read the junction
sits at the length of the second part from the read's start.

| Kind | Reads | Fields | Built from | Purpose |
|---|---|---|---|---|
| `lina` | 500 | `<start>_<stop>` | strain A of `[1000000, 1200000)`, about 30× | primary contig |
| `linb` | 333 | `<start>_<stop>` | strain B: the same region with a substitution at every offset from the region's start where offset mod 150 = 75, about 20× | alternate contig |
| `circ` | 67 | `<start>_<length>` | a read starting at `<start>` in the 40 kb circle `[2000000, 2040000)`, wrapping past its end | circular contig |
| `circb` | 50 | `<start>_<length>` | as `circ`, from a variant of the circle with a substitution at every offset from the circle's start where offset mod 1000 = 7 | duplicated contig |
| `low` | 13 | `<start>_<stop>` | `[3000000, 3060000)`, about 2.5× | low-depth contig |
| `iso` | 3 | `<start>_<stop>` | three non-overlapping reads from `[3300000, 3400000)` | reads myloasm assembles into nothing; absent from every output |
| `chim` | 20 | `<a_start>_<a_stop>_<c_start>_<c_length>` | strain A `[a_start, a_stop)` joined to the circle read `<c_start>_<c_length>` | chimera splits at the junction |
| `ovh` | 20 | `<start>_<stop>_<tail_start>` | 9 kb of strain A `[start, stop)`, then 3000 bp from `[tail_start, tail_start + 3000)`, which no other read covers | a split that leaves a partial alignment, in both orientations |

A substitution replaces A, C, G and T with C, G, T and A.

### What myloasm produced

| Contig | File | Origin | Notes |
|---|---|---|---|
| `u491ctg` | primary | strain A | 196340 bp polished, `LN` 196466 |
| `u826ctg` | alternate | strain B | `secondary_to` `u491ctg`, ANI 99.18 |
| `u912ctg` | primary | low-coverage region | depth 2 at every tier |
| `u982ctg` | primary | the circle variant `circb` | `circular-yes`, self-link `L` |
| `u840ctg` | duplicated | the circle `circ` | `secondary_to` `u982ctg`, ANI 99.91; `circular-yes`, self-link `L` |

The PAF has 1085 rows over all five contigs. The round-1 breakpoint log has 40 lines, one
breakpoint each:
- 20 chimeras with `COND:1`
- 10 forward overhang reads with `COND:-3`
- 10 reverse overhang reads with `COND:-2`

PAF read names carry no suffix, `+split0` or `+split1`.

**Two myloasm behaviours do not occur in this fixture:**
- **A contig in the GFA but in no FASTA.** myloasm drops a contig this way when, after the
  GFA is written, more than 10% of it has no read coverage at depth ≤ 5, or when polishing
  skips it. Clean simulated reads did not trigger either case.
- **Round-2 breakpoints, and the two-level `+splitA+splitB` names they produce.**

### Format facts these files show

These are checked against myloasm 0.7.0's source.
- PAF tags are untyped (`shared_mini:N`, `diff_snp:N`, `shared_snp:N`).
- For `-` alignments, PAF query coordinates are on the reverse complement of the read piece.
- Columns 10 and 11 of the PAF are anchor spans, not residue matches and block length.
- `a`-line slices tile each contig exactly: offsets accumulate slice lengths up to `LN`.
- `a`-line read ranges are on the reverse complement for `-` reads.
- Contig coordinates in the GFA and PAF are on the unpolished contig. The FASTA sequences
  are polished, and circular contigs are trimmed: `u840ctg` is 51274 bp unpolished and
  40000 bp polished.

## Regenerating

Generated with **myloasm 0.7.0** (bioconda build `h7f49ad2_1`, linux-64). Run it in a
directory outside the repository that holds the decompressed reads, so the command line the
log records carries no machine paths. One thread, because with more myloasm writes the PAF and
the breakpoint logs in thread order. From the repository root:

```bash
conda create -n myloasm-070 -c conda-forge -c bioconda myloasm=0.7.0=h7f49ad2_1   # linux-64
REPO=$PWD
mkdir ../myloasm_work && cd ../myloasm_work
gzip -dc "$REPO/data/myloasm/ecoli_sim_reads.fa.gz" > reads.fa
conda run -n myloasm-070 myloasm reads.fa -o run1 --hifi -t 1

DEST=$REPO/data/myloasm/ecoli_sim
for f in assembly_primary.fa alternate_assemblies/assembly_alternate.fa \
         alternate_assemblies/duplicated_contigs.fa final_contig_graph.gfa; do
    gzip -9 -n -c "run1/$f" > "$DEST/$f.gz"
done
for f in 3-mapping/map_to_unitigs.paf.gz 0-cleaning_and_unitigs/read_coverages.txt.gz \
         0-cleaning_and_unitigs/remap_temp/read_coverages.txt.gz; do
    cp "run1/$f" "$DEST/$f"
done
sed -E 's/(SYSTEM HOST NAME: ).*/\1redacted/' run1/myloasm_*.log | gzip -9 -n > "$DEST/myloasm.log.gz"
git -C "$REPO" diff --stat data/myloasm/   # expect a diff only in myloasm.log.gz
```

With gzip 1.13, these commands reproduced every committed file except the log byte for byte.
The log differs only in its timings and memory figures. Two runs from the same reads gave
identical output. Another gzip can compress differently, so compare decompressed contents when
`git diff` reports a change. Other platforms have their own 0.7.0 builds (osx-arm64
`h2797cb0_1`, osx-64 `h565ea35_1`, linux-aarch64 `ha2fee11_1`); for those, drop the build
string. Whether those builds reproduce these files exactly has not been checked.
