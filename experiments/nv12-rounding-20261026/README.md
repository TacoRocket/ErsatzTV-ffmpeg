# Archived NV12 rounding experiment — 2026-10-26

Status: **unshipped and incomplete**. This directory preserves the original
candidate patch without adopting it into the current Match/Native stack.
No build recipe applies patches from this experiment directory. The Linux amd64
recipe applies the separate top-level `patches/*.patch` files and explicitly
named `patches/libplacebo` patches; this archive changes neither list.

## Mechanism and intended correction

The older producer-storage path tried to preserve an intermediate half-float
rounding step while writing Y directly. On the tested software Vulkan backend,
the pack/unpack comparison was optimized away, so the adjustment did not run.
`candidate-decode.patch` changes `src/renderer.c` inside `inline_nv12_output`:
it decodes the packed half-float magnitude using exponent and fraction bits for
the comparison, retaining the existing pack, decrement, and final unpack.
This is a compiler-workaround experiment, not a new output mode or a claim of
portable half-float attachment behavior.

The file is copied byte-for-byte from the original scratch candidate. SHA-256:

```text
be590cb86816ae61292820a4b0151de44300215f7bbe1042d69fb84ae02bb1fd
```

## Source provenance and limits

The earlier software investigation's source identity records libplacebo
`cee9b076f2c63104ccfd497fa79c39a867293ec4` and this fork's producer-storage
revision `827d33da414992798594eff7b06c959bd54cfe3a`, using a Git archive that
excluded pre-existing local renderer edits. These identify the investigation's
source context, not a newly verified exact apply-base for this candidate.

The rounding report instead refers to `intel-pattern-check/source-identity.json`.
That referenced identity file was absent from the retained local artifacts during
this archival pass. The report also records a Vulkan 1.3 test-harness cap.
Consequently the complete original source/harness reconstruction remains
unverified. This archive does not assert that the patch applies unchanged to
current main or to the rewritten Native layer. Recovering that exact base and
checking application would be a separate investigation before any adoption.

## Recorded evidence, not fresh validation

The original report and summaries record these results with Mesa 25.2.8 and
glslang 15.1.0:

- In the reduced 16,384-value finite-normal test, the corrected software output
  and Intel original/corrected output matched the independent CPU truncation
  calculation. The software original missed 8,191 adjustments.
- Intel full-render output stayed byte-identical, with the candidate exiting 0.
- Software full-render differing Y samples fell from 296,785 to 2,592 over four
  cases. Each residual difference was at most one 8-bit code value; UV was exact.
  The software candidate intentionally exited 2 because exact parity still failed.
- Baseline frame hashes remained unchanged, and the optimized path retained the
  recorded four-to-three pass reduction. This does not establish a speed gain:
  no controlled performance run of the correction was completed.

The numerical test covered the demonstrated finite normal values, not exhaustive
NaN, infinity, subnormal, or cross-driver behavior. The report attributed the
remaining full-render differences to the skipped 1:1 linear Y read; that is a
historical diagnostic conclusion, not new proof produced by this archive.
Intel parity and a corrected reduced case do not establish complete software
Vulkan parity or justify changing the current runtime. No public runtime patch,
container, deployment, or existing performance claim is changed here.
