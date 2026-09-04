# Remaining tasks

Written 2026-09-03, after Phase 6. Continues on a Windows machine.

**Updated 2026-09-04: group A is closed.** The Windows machine turned out to run
WSL2 Linux, which is where the build happens, so the toolchain items were already
satisfied. The gate passes here and the throughput baseline is in A8. The next
blocking thing is not a task in this file: it is the five decisions in
`docs/PUBLISHING-PLAN.md` section 8, which gate all of F and G.

This file is the working list. `specs/02-solution-proposal.md` stays the design
authority, and section 13 of it stays the locked decisions. Where this file and
the spec disagree, the spec wins and this file is wrong.

---

## 0. Where things stand

Seven of the spec's ten phases are built and green: phases 0 to 6 of section 11,
executed in a different order than numbered there. The C core is at 465
assertions, three sanitizers, four fuzz targets, warning free. The JS binding,
the view and React packages, and the Java JNI binding all produce byte identical
output to the native CLI across 240 comparisons.

Not built: Phase 7 (SIMD), Phase 8 (view polish), and parts of the spec's Phase 6
server profile. Nothing is published to any registry.

The task groups below are ordered by value, not by phase number. Group A is
first because nothing else can run until it is done. Group B is second because it
is the only group where being wrong costs a customer rather than costs time.

---

## A. Get the Windows machine building and gating

**Closed 2026-09-04. The gate passes here and the baseline is recorded.**

The premise of this group turned out to be wrong in the way that made it cheap.
It was written expecting native Windows; the machine runs **WSL2 Linux** (Oracle
Linux 9, kernel 6.6.87.2-microsoft-standard-WSL2) on that Windows box, and the
August build tree is intact on it. So the three items about MSVC, the LLVM
distribution and `/dev/null` never applied, and the toolchain was already
installed and already correct.

Anything below marked **N/A (WSL2)** becomes live again only if someone builds
outside WSL, on native Windows. Do not delete those three: they are the record of
what native Windows would still need.

- [x] **A1. Python venv. Not needed, and that is the finding.** Every python
      script in `scripts-and-commands/` and `core/fixtures/` imports the standard
      library only: `argparse`, `json`, `os`, `pathlib`, `platform`, `random`,
      `re`, `shutil`, `subprocess`, `sys`, `tempfile`, `time`. There is nothing to
      install, so there is nothing to activate. The system `python3` (3.9.25) runs
      the whole gate. No virtualenv is present and none is required. **If a script
      ever grows a third party import, this item reopens.**

- [x] **A2. JDK.** `java 25.0.1 2025-10-21 LTS` (Oracle GraalVM). `JAVA_HOME` is
      **unset**, which is fine here: Maven resolves the JDK from `java` on
      `PATH`, and `build_jni.py` derives the JNI include directory from the same
      place. `<release>21</release>` is satisfied. The mac's failure mode, a
      `JAVA_HOME` pointing at 17 while 21 and 25 were installed, cannot occur
      while the variable is unset.

- [x] **A3. A C compiler that takes clang or gcc flags.** N/A (WSL2).
      `cc` is gcc 11.5.0 and `clang` is 21.1.8; both take the flag set. MSVC is
      not in the picture. Live again only for a native Windows build.

- [x] **A4. A clang with the WebAssembly backend, plus `wasm-ld`.** N/A (WSL2).
      `clang 21.1.8` and `LLD 21.1.8` `wasm-ld` are both on `PATH`, and both wasm
      modules build and pass their smoke check in the gate below.

- [ ] **A5. Fix the wasm toolchain probe for Windows.** N/A (WSL2), and
      **deliberately left open rather than fixed blind.** `core/Makefile` line 113
      still probes with `-x c -c /dev/null -o /dev/null`, which works here and
      would fail on native Windows. The fix is a temporary file for both operands,
      or `NUL` guarded by an OS check, but it cannot be verified on this machine,
      and an unverifiable change to the build system that the determinism gate
      runs through is a poor trade for a platform nobody is building on. Do it
      when someone actually builds outside WSL, and test it there.

- [x] **A6. Node 20+ and pnpm 9.12.** Node v24.13.0. pnpm resolves through
      corepack against the `packageManager` pin in `js/package.json`.
      `js/node_modules` is present and all three package suites pass.

- [x] **A7. Maven.** 3.9.12.

- [x] **A8. The gate passes, and here is the baseline.**
      `python3 scripts-and-commands/run_phase6_checks.py --quick` exits 0 with
      `gate passed` in all three sections: 465 C assertions under four
      configurations, seven fuzz targets at 200,000 inputs each, 62 binding, 83
      view, 28 react and 132 Java tests, and **210 byte identical comparisons**
      across native, wasm, wasm+simd and Java. 210 rather than 240 because
      `--quick` skips the 15 MB pair; run it without `--quick` before any release
      commit.

      **Parse throughput on this machine, which is what Phase 7's D6 is measured
      against:**

      | | this machine | `docs/HANDOFF-phase6.md` 6.4 |
      |---|---|---|
      | parse, zero copy | **717 MB/s** (20.93 ms, 98.2 M cells/s) | 723 |
      | parse, streamed | **576 MB/s** (26.06 ms) | 569 |
      | parse + digests, zero copy | **502 MB/s** (29.87 ms) | 496 |
      | parse + digests, streamed | **405 MB/s** (37.04 ms) | 385 |

      This item expected the numbers not to match, because it expected different
      hardware. **They match to within a few percent, so it is the same hardware**
      and the August measurements carry over rather than being superseded. It also
      confirms the ground for ordering group D last: the scalar parser is 2.4x
      above the spec's 300 MB/s floor.

- [x] **A9. Line endings are pinned, and nothing was wrong to begin with.**
      Two findings. First, **no fixture or golden file is tracked at all**: every
      one is generated by `core/fixtures/gen_fixtures.py` into a gitignored
      directory, so the corruption this item feared had no tracked file to happen
      to. Second, all 202 tracked files are already LF, verified by the new
      `scripts-and-commands/check_line_endings.py`.

      A `.gitattributes` now pins it rather than leaving it to whatever
      `core.autocrlf` is set to on the next machine. It carries one rule this item
      did not anticipate: **`*.csv` and `core/fixtures/generated/**` are marked
      `-text !eol`, receiving no conversion at all**, because
      `core/fixtures/generated/crlf.csv` carries CRLF *as the data under test*.
      A blanket `text=auto` would normalize away the thing that fixture exists to
      prove. It is gitignored today, so this is insurance against H3.3 publishing
      the fixtures later.

---

## B. Confirm the five flagged assumptions

These are implemented, unconfirmed, and every one of them fails silently: the
diff returns a wrong answer that looks like a right answer. `docs/HANDOFF-phase6.md`
section 8 has the full statement of each. Six phases of "flag it, do not
re-decide it" was correct during construction and is not correct any more.

The check for all five is the same: take one real pair of files, and for the two
JDBC ones one real query, run the diff, and look at whether the report says what
you would say.

**2026-09-04: the evidence is gathered and the five are now answerable.**

```bash
python3 scripts-and-commands/confirm_assumptions.py
```

`docs/ASSUMPTIONS-B.md` is the write up: what each rule does, the actual engine
output beside what the alternative would have produced, the question, and the cost
of changing the answer. The harness is
`java/src/test/java/com/ibhatech/csvdiff/AssumptionsMain.java`, which drives the
real engine through the real binding rather than restating the rules in prose.

**It decides nothing, and that is deliberate.** Each of the five ends in a question
whose answer is a fact about the data a deployment really holds, and that fact is
not in this repository. What was in the repository, the behaviour, is now visible
instead of described.

Two things the run turned up that were not in the statements below:

1. **B2 is worse than "a different format breaks it".** A CSV carrying
   `2026-01-31T14:22:05.000`, the same instant in the same ISO layout, still
   reports as changed, because this renderer drops a zero fraction while a fixed
   precision export writes three digits. That is the likeliest way to hit B2 while
   everything looks correct to the eye.
2. **B4 has a concrete failure, not a theoretical one.** In a file whose first data
   record spans two physical lines, the duplicate on physical line 8 is reported as
   "rows 5 and 7", and line 7 holds a different row with a different key. The
   record count also includes the header rows, which is why a first data row
   reports as row 5.

**ALL FIVE ANSWERED BY MANAS 2026-09-04.** Four were right as built; one was a
defect and is fixed. `docs/ASSUMPTIONS-B.md` carries the evidence, the answers and
the reasoning behind each.

- [x] **B1. SQL NULL renders as an empty field. CONFIRMED as built.** An empty
      field is what the export writes. The detail that came with the answer is
      verified rather than assumed: a null in the last column is written as a
      trailing delimiter, and `1,ann,` under LF, under CRLF, and written as an
      explicit empty quoted field all produce three columns and one unchanged row.

- [x] **B2. Timestamp text format. REJECTED, and fixed.** An absent fraction must
      equal `.000`, and the general rule was confirmed, so trailing zeros in a
      fraction are not significant at any width. This was a defect rather than a
      decision: it reported the same instant in the same ISO layout as a changed
      cell.

      Fixed by `ibha_canonical_timestamp` in `core/src/normalize.c`, beside
      `ibha_canonical_decimal`, with a `TIMESTAMP` branch in `ibha_normalize`. Not
      gated on an option, because two timestamps differing only in trailing
      fractional zeros are the same instant, which is what the type means.
      **No ABI change:** it rides on `IBHA_CSVD_DATE_EXACT`, whose documented
      meaning is now byte equality on the canonical form, and `date_compare` was
      already folded into `compare_id`. The date is still not parsed, so
      `31/01/2026` still differs from `2026-01-31`; that stays `DATE_VALUE` and
      stays unimplemented.

- [x] **B3. The asymmetric ragged row rule. CONFIRMED as built, both halves.**
      Forgiving extra trailing empty columns is agreed and refusing short rows is
      agreed. `row_end` is unchanged. It is consistent with the B1 answer rather
      than merely compatible with it: a producer that writes a null last field as
      a trailing delimiter never emits a short row.

- [x] **B4. Duplicate key row numbers are record based. CONFIRMED as built.** The
      CSVs do contain embedded newlines, and a record spanning two lines is still
      one row: lines 2 and 3 together are record 2, and line 4 is record 3, which
      is what the engine counts. The record is the unit a person means by "row".

- [x] **B4a. Also report the physical line. DECIDED 2026-09-04: not needed.**
      **Excel is the viewer, and Excel row numbers are record numbers.** Excel
      parses the CSV properly, so a quoted field containing a newline is one grid
      row with a line break inside the cell. A four row header is Excel rows 1 to
      4 and the first data record is Excel row 5, which is what the engine already
      calls record 5. Adding physical line numbers would have added the number
      that does *not* match what the reader is looking at.

      That also disposes of the cost: an array on the public `ibha_csvd_table`,
      four bytes a row, regenerated wasm offsets, a JNI accessor and the number
      carried through four emitters and both bindings, all avoided.

      **One case does drift, verified rather than assumed:** a blank line in the
      middle of a file, because the engine treats a wholly empty line as not a
      record while Excel shows it as an empty grid row. Each such blank line puts
      the engine one further behind. A trailing blank line at end of file causes
      no drift, since no record follows it.

      Not fixable by counting blank lines as records: a blank line is one empty
      field, which in a file of three or more columns is a short row, and B3
      refuses short rows. So the choice is skipping them, which is what happens,
      or refusing any file that contains one. `stats.blank_lines` is already
      public in the C ABI and already in the JS struct offsets, so a caller who
      cares can explain the drift without an engine change.

- [x] **B5. `VARCHAR(n)` counts characters, not bytes. CONFIRMED as built.** The
      data is UTF-8 string values and a byte limit is not what `VARCHAR(5)` means.
      The question that came with the answer, whether this matters to the C
      engine, has a short answer: it **is** the C engine. `char_count` in
      `core/src/validate.c` counts UTF-8 lead bytes, and both bindings inherit it
      because both run this engine.

- [x] **B6. Move each confirmed assumption into the spec as a decision.** Done, in
      `specs/02-solution-proposal.md` section 13, so phase 8's handoff does not
      carry the same five items.

---

## C. Reconcile the spec with what was built

- [ ] **C1. Decide the row feed, then make one document true.** Spec 13.1 is a
      locked decision titled "row feed: build it" and says the columnar index has
      two front ends. There is no row feed in the ABI. `docs/HANDOFF-phase6.md`
      section 5.2 records the reversal and its reasoning: a second front end
      means a second set of tests, fuzz targets and determinism coverage, and it
      reopens a core unchanged since Phase 4. The Java side encodes rows as RFC
      4180 into a staging buffer instead, behind `DiffSource`, so a native feed
      can replace it invisibly later.

      Either build it, or amend 13.1 to record that it was traded away and why.
      Leaving a locked decision contradicted by a handoff means the next reader
      of 13.1 goes looking for an ABI that does not exist.

- [ ] **C2. Reconcile the phase numbering.** Spec section 11 numbers the phases
      differently from the order they were executed (the build did core first,
      view later). Both agree that Phase 7 is SIMD, so nothing downstream breaks,
      but section 11 currently describes a plan that was not followed. Rewrite it
      to match, with a line saying it was resequenced.

- [ ] **C3. Record what the spec's Phase 6 dropped.** N-API addon, Python wheels,
      Go cgo binding, `mmap` ingest, and the batch driver deliverable ("500 pairs
      against an RDBMS source with bounded per worker memory") are all in spec
      section 11's Phase 6 and none exist. Decide for each: wanted, or dropped.
      Section 13.12 already dropped the FFM binding explicitly; these five were
      dropped silently.

---

## D. Phase 7, the SIMD parser

`docs/HANDOFF-phase6.md` section 11 carries a ready-to-paste prompt for this
phase. Start there rather than from this list.

Worth knowing before starting: the scalar parser measured 723 MB/s zero copy
against a spec floor of 300 MB/s, and handoff section 6.4 says outright that SIMD
here is a throughput improvement rather than a rescue. Groups B and C buy more.

- [ ] **D1.** SIMD structural scan: `pclmulqdq` and AVX2 or NEON on native, the
      emulated prefix XOR on wasm, selected by runtime feature detection.
- [ ] **D2.** The scalar parser is retained permanently as the differential
      oracle, per spec 13.11. A property test asserts the two produce identical
      indexes on every fixture and every fuzz input.
- [ ] **D3.** `check_determinism.mjs` must still pass. It compares native,
      scalar wasm, SIMD wasm and Java byte for byte and it passes today, which is
      what makes its first failure mean something. It is the gate that catches a
      quoted field straddling a 16 byte lane.
- [ ] **D4.** If runtime feature detection needs a new ABI entry point, it is the
      first ABI change since Phase 3 and both bindings need it.
- [ ] **D5.** A new C source file must be added to both `core/Makefile`'s
      `CORE_SRC` and `build_jni.py`'s `CORE_SOURCES`. The script asserts the two
      lists match and fails the Java build if they diverge.
- [ ] **D6.** Deliverable: measured 2 to 4x parse speedup, native ahead of wasm,
      identical output, against the A8 baseline.

---

## E. Phase 8, view polish

Spec section 8.5's table, versions 0.2 through 0.5. Everything at 0.1 shipped:
vertical virtualization, sticky header and key columns, single scroll container,
and all four layouts of 8.4 including `sideBySide`. `docs/HANDOFF-phase5.md`
section 7 explains what each deferred item needs.

- [ ] **E1.** Keyboard navigation and focus management (0.2). Arrow keys, page
      keys, jump to next change. Nothing exists for this today.
- [ ] **E2.** Column resize by drag (0.2). The view model already owns widths,
      applies them as custom properties and has `setColumnWidth`. What is missing
      is the drag handle in the React component: a pointer event handler and no
      new state.
- [ ] **E3.** Column virtualization (0.3), for 200+ column files.
- [ ] **E4.** Variable row height for multiline cells (0.3). Needs a measured
      offset table, which is also what non-uniform stacked rows need, so E3 and
      E4 arrive together.
- [ ] **E5.** Column pinning, hide and show, reorder in view (0.4). View only,
      does not touch diff semantics.
- [ ] **E6.** Sort and filter by column and change kind (0.4). Sorting overrides
      the target order constraint, so it is opt in and must be visibly indicated.
      That is a design decision nobody has made yet.
- [ ] **E7.** Export visible or full diff to CSV, XLSX, Markdown (0.4). Streamed
      from wasm, never materialized. `handle.emit` already does the work; the
      honest version waits on E9.
- [ ] **E8.** Print stylesheet (0.5) and a formal accessibility audit (0.5).
- [ ] **E9.** A resumable emitter, in the engine. `ibha_csvd_emit` is all or
      nothing, so every binding sizes with one pass and fills with a second, and
      a large report is produced twice. This has been deferred since Phase 4 and
      is engine work, not binding work. E7 depends on it being done properly.
- [ ] **E10.** A demo app that diffs two dropped files. Spec section 11 lists
      this as the deliverable of its Phase 3 and it was never built. It is also
      the cheapest way to exercise E1 through E8 by hand.

---

## F. Publishing the npm packages

**PUBLISHED 2026-09-04. All three are live at 0.1.0 under Apache-2.0.**

```
@ibhatech/csvdiff-core    0.1.0   dependencies: {}
@ibhatech/csvdiff-view    0.1.0   @ibhatech/csvdiff-core: ^0.1.0
@ibhatech/csvdiff-react   0.1.0   @ibhatech/csvdiff-core: ^0.1.0, @ibhatech/csvdiff-view: ^0.1.0
```

Published by hand from a terminal rather than by CI, which is the right way round
for a first release: nothing in this repository holds an npm token, and the OTP
prompt is the last chance to notice something wrong before a permanent action.

**Verified from the registry, not from the workspace**, which is the whole point of
F5: installing only `@ibhatech/csvdiff-react` in an empty project pulled `view` and
`core` transitively through the caret ranges and deduped them, a Vite build against
those three succeeded, and both wasm modules were emitted as hashed assets. All
three carry `LICENSE` and `NOTICE`; `core` ships `wasm/` and `view` ships `styles/`.

Two things this confirmed had made it into what was actually published, rather than
only into the working tree: the `workspace:^` caret, which resolved to `^0.1.0`
rather than an exact pin, and the `workerClient` fix, which is why a consumer build
no longer warns about a missing `worker.ts`.

**0.1.0 is now permanent for all three.** Any correction is 0.1.1.


Three packages publish, in dependency order: `@ibhatech/csvdiff-core`, then
`@ibhatech/csvdiff-view`, then `@ibhatech/csvdiff-react`. All are at `0.1.0`.
`@ibhatech/csvdiff-monorepo` is `"private": true` and must stay that way.

### F1. Decisions to make before any command runs

- [x] **F1.1. Licence. DECIDED 2026-09-04: Apache-2.0.** All three say `"license": "UNLICENSED"` and there is no
      `LICENSE` file anywhere in the repo. Publishing to the public registry
      under `UNLICENSED` tells consumers they may not use it, which makes the
      publish pointless. Pick a licence, add the file, set the field in all
      three. If this is meant to be private, that is a different route: see F6.
- [x] **F1.2. Public or private. DECIDED 2026-09-04: public.** `@ibhatech` is a scoped package and npm
      defaults scoped packages to **restricted**. A public publish needs
      `--access public` on the first publish of each package, or better,
      `"publishConfig": { "access": "public" }` in each `package.json` so it is
      not a flag someone forgets.
- [ ] **F1.3. Own the scope.** `@ibhatech` must exist as an npm organization or
      user scope and your account must be a member. Create it before publishing;
      the first publish fails otherwise.

### F2. Manifest work

- [ ] **F2.1.** Add `repository`, `homepage`, `bugs` and `author` to all three
      manifests. npm shows a package with no repository link as unmaintained by
      default and it is the first thing anyone evaluating it looks for.
- [ ] **F2.2.** Confirm each `files` array is complete. `core` ships
      `dist`, `wasm`, `README.md`; `view` ships `dist`, `styles`, `README.md`;
      `react` ships `dist`, `README.md`. The `wasm` and `styles` entries are the
      ones worth checking, since neither is produced by `tsc`.
- [ ] **F2.3.** Add a `LICENSE` file to each published package directory, or set
      up the publish to copy the root one in. npm does not inherit it from a
      parent directory.

### F3. The two things that will break the publish

- [ ] **F3.1. The `workspace:*` protocol.** `view` depends on
      `"@ibhatech/csvdiff-core": "workspace:*"` and `react` depends on both.
      That specifier is not valid on the registry. **`pnpm publish` rewrites it
      to the real version at pack time; `npm publish` does not and will publish
      a broken manifest.** Use `pnpm publish` and never `npm publish` for these.
      Verify with `pnpm pack` and read the generated tarball's `package.json`
      before the first real publish.

- [ ] **F3.2. The wasm artifacts are gitignored.** `js/.gitignore` excludes
      `packages/*/wasm/*.wasm` and `dist/`. A fresh clone plus `pnpm publish`
      therefore ships `@ibhatech/csvdiff-core` with no engine in it, and the
      failure surfaces at the consumer's first import, not at publish time. Add
      a `prepublishOnly` script to `packages/core` that runs the wasm build and
      the tsc build and **fails if `wasm/*.wasm` is missing**. This is the single
      most likely way to ship a broken package.

### F4. The publish itself

- [ ] **F4.1.** From a clean tree on the release commit:
      `pnpm -r build`, then `pnpm run wasm`, then the full gate.
      Do not publish from a tree with uncommitted changes.
- [ ] **F4.2.** `pnpm pack` in each package. Extract each tarball and read what
      is actually inside: the `dist`, the `.d.ts` files, the wasm, the styles,
      the rewritten dependency versions. Five minutes here saves a patch release.
- [ ] **F4.3.** `npm login`, with 2FA enabled on the account. For CI, use a
      granular access token scoped to these three packages rather than a classic
      automation token.
- [x] **F4.4. Done 2026-09-04.** Published in dependency order: `core`, then `view`, then `react`.
      Each waits for the previous to be resolvable on the registry, which is not
      instant.
- [ ] **F4.5.** Consider `--provenance` if publishing from GitHub Actions. It
      signs the package with a verifiable link back to the source commit and
      costs nothing but a workflow permission.
- [ ] **F4.6.** Tag the release commit and push the tag.

### F5. After publishing

- [x] **F5.1. Done.** In a scratch directory outside this repo, `npm install
      @ibhatech/csvdiff-react` and diff two files. A consumer installing from the
      registry exercises paths that a workspace install never touches, notably
      how the wasm file is located at runtime.
- [ ] **F5.2.** Check the rendered README on npmjs.com for each package.
- [x] **F5.3. Done.** Verify the packages work under a bundler (Vite) and under plain
      Node, since `core` ships both a main entry and a `./worker` entry.

### F6. If these are meant to be private instead

Skip F1.2 and publish with `"access": "restricted"` to the npm registry under a
paid organization, or point `publishConfig.registry` at a private registry
(GitHub Packages, Artifactory, Verdaccio). The rest of F2 through F5 is
unchanged. Decide this before F1.1, because a private package does not need a
public licence.

---

## G. Publishing the Java library to Maven

Coordinates today: `com.ibhatech:ibha-csvdiff-java:0.1.0`, jar packaging, Java 21
bytecode, `Automatic-Module-Name: com.ibhatech.csvdiff`. Sources and javadoc jars
are already configured and attached.

### G1. Decisions to make first

- [x] **G1.1. Which repository. DECIDED 2026-09-04: Maven Central.** Maven Central (public, permanent, most work) or
      a private repository (Artifactory, Nexus, GitHub Packages, AWS
      CodeArtifact). The steps below are for Central; a private repository needs
      only G3.1, G4 and a `<distributionManagement>` block.
- [ ] **G1.2. Namespace ownership.** Publishing under `com.ibhatech` on Central
      requires proving you control `ibhatech.com`, via a DNS TXT record. If you
      do not own that domain, either use one you do own or switch the groupId to
      `io.github.<your-github-username>`, which is verified by owning the GitHub
      account. **Decide this before the first publish: changing groupId after
      release means republishing under new coordinates, since Central releases
      are immutable.**
- [x] **G1.3. Licence. DECIDED 2026-09-04: Apache-2.0, same as F1.1.** Central requires a `<licenses>` block and it must be a
      real licence. Same decision as F1.1; make it once for both ecosystems.

### G2. The native library problem, which is the real work here

`NativeLibrary.java` looks for five platforms: `linux-x86_64`, `linux-aarch64`,
`darwin-aarch64`, `darwin-x86_64`, `windows-x86_64`. `build_jni.py` builds
exactly one, the host's. A jar published from your Windows machine works only on
Windows x86-64, and fails at first use everywhere else with a message naming the
four it could not find.

- [x] **G2.0. Which platforms. DECIDED 2026-09-04: `linux-x86_64` and
      `darwin-aarch64`.** The two Manas runs and can test. `NativeLibrary`'s
      error message currently claims all five are bundled, which must be
      corrected before release or a Mac Intel or Windows consumer goes looking
      for a packaging fault instead of an unsupported platform.

- [ ] **G2.1. Decide the artifact shape.** Either one fat jar carrying all five
      libraries under `native/<os>-<arch>/`, which is what `NativeLibrary`
      already expects and what `pom.xml`'s resource block is built for, or a thin
      jar plus five classifier artifacts that consumers select. The fat jar is
      simpler for consumers and is the shape the code assumes; keep it unless the
      size becomes a complaint.
- [ ] **G2.2. Build a CI matrix**, one runner per target. GitHub Actions covers
      `ubuntu-latest` (x86_64), `ubuntu-24.04-arm` (aarch64),
      `macos-latest` (darwin-aarch64), `macos-13` (darwin-x86_64) and
      `windows-latest` (x86_64). Each runs `python scripts-and-commands/build_jni.py
      --out <staging>` and uploads its one library as an artifact.
- [ ] **G2.3. A collect step** that downloads all five into `java/target/native/`
      before `mvn package` runs, so the existing resource block picks them all up.
- [ ] **G2.4. Test the fat jar on at least two platforms** before release. The
      extraction path in `NativeLibrary` is the part that a single-platform build
      never exercises.

### G3. pom.xml additions required by Central

The current pom has none of these. Central's validation rejects the upload
without them, so add them all in one pass.

- [ ] **G3.1.** `<url>`, and a `<scm>` block with `connection`,
      `developerConnection` and `url`.
- [ ] **G3.2.** A `<licenses>` block matching G1.3.
- [ ] **G3.3.** A `<developers>` block with at least one entry.
- [ ] **G3.4.** GPG signing. Generate a key, publish the public half to a
      keyserver, and add `maven-gpg-plugin` bound to `verify`. Every artifact,
      including the sources and javadoc jars, must be signed. Keep the private
      key and passphrase in CI secrets, never in `pom.xml`.
- [ ] **G3.5.** The publishing plugin. Central moved to the Central Portal;
      `central-publishing-maven-plugin` is the current path. The older
      `nexus-staging-maven-plugin` and `oss.sonatype.org` route is retired, so
      ignore any guide that mentions it.
- [ ] **G3.6.** Confirm the javadoc build is clean. The build already runs
      `-Xlint:all -Werror` for compilation, but javadoc has its own errors and
      Central rejects a javadoc jar that failed to build. Run `mvn javadoc:jar`
      once and fix what it reports.

### G4. Credentials

- [ ] **G4.1.** Register the namespace at central.sonatype.com and complete the
      DNS or GitHub verification from G1.2.
- [ ] **G4.2.** Generate a user token in the Central Portal and put it in
      `~/.m2/settings.xml` as a `<server>` entry. Never in the repo.
- [ ] **G4.3.** Add the same as CI secrets, along with the GPG key and
      passphrase.

### G5. Release

- [ ] **G5.1.** Decide whether `0.1.0` is the right first public version. It
      signals "expect breaking changes", which is honest here given E9 and C1 are
      both outstanding and both could change public API.
- [ ] **G5.2.** Publish a `-SNAPSHOT` first and consume it from a scratch project.
- [ ] **G5.3.** `mvn deploy` the release. **Central releases are permanent and
      cannot be deleted or overwritten.** Verify the staged contents in the
      Portal before releasing.
- [ ] **G5.4.** Tag the commit. Confirm the artifact appears on Maven Central
      search, which lags the publish by a few hours.
- [ ] **G5.5.** Document the consumer-side `--enable-native-access=ALL-UNNAMED`
      requirement for JDK 24+ in the published README, not only in
      `java/README.md`. JDK 24 warns on native library loading and a later
      release will refuse it.

---

## H. Splitting java and js into their own repositories

`docs/HANDOFF-phase6.md` blocker 9.6 has flagged this for six phases and says the
cost of deciding late keeps rising. It is right, and it is a decision rather than
a task.

### H1. What actually ties the three directories together today

Read this list before choosing, because it is the whole cost of the split:

1. `scripts-and-commands/build_jni.py` compiles `../core/src/*.c` directly, and
   reads `../core/Makefile` to assert its `CORE_SOURCES` list matches `CORE_SRC`.
   That assertion is deliberate: it stops the Java binding from quietly running a
   different engine than the C tests cover.
2. `java/pom.xml` runs that script from `${project.basedir}/../scripts-and-commands/`.
3. The Java tests read `../core/fixtures/generated` by relative path, set through
   the `ibha.csvdiff.fixtures` system property in the surefire config.
4. `js/package.json`'s `wasm` script runs `make -C ../core wasm` and then copies
   the output into `packages/core/wasm/`.
5. `scripts-and-commands/check_determinism.mjs` drives the native CLI, both wasm
   builds and the compiled Java classes from one process, and compares all four
   byte for byte. **This is the project's most valuable single test** and it is
   the thing a split breaks most severely: after a split, no single repository
   can run it.

### H2. Recommendation, and the decision

**DECIDED 2026-09-04 by Manas: one repository. This question is closed.** Six
phases of flagging it are over; H3 below is retained as the record of what a split
would have cost, not as a plan anybody should execute.

The variant actually proposed was better than the one H3 describes, and it is worth
writing down why it still lost, because it is the version that will be proposed
again:

> Keep this repository private as an umbrella holding the docs and specs, and make
> `core`, `java` and `js` public submodules of it.

Three things in its favour, all real. The direction is the safe one: a private
parent with public submodules works, whereas a public parent with private children
breaks every outside clone. It dodges H3.2's worst problem, because the umbrella
has all three submodules checked out at pinned SHAs and can therefore still run
`check_determinism.mjs` against all four producers, rather than against the last
*released* bindings. And the relative paths in H1 all survive, because submodules
land at `core/`, `java/` and `js/` exactly where the directories are today.

**What sank it: a split only pays off if each repository is independently useful,
and `java` is not independently buildable.** `build_jni.py` compiles core's C
sources directly and asserts its file list against `core/Makefile`. Clone `java`
alone and it cannot build, which is the entire purpose of having split it. Fixing
that needs one of two bad options: nest `core` as a submodule inside both `java`
and `js`, at which point the two pinned core SHAs can drift apart and silently
break the exact guarantee the determinism gate exists to prove; or give `core` a
real release process publishing the header, both wasm modules, five native
libraries and checksummed fixtures, which is more work than the whole publishing
plan combined.

Two smaller costs, recorded because they were underweighted the first time. A
cross-cutting change becomes three commits plus a fourth to bump the pointers, and
the gate cannot run until all four land; **Phase 7 is exactly such a change**,
since a new SIMD source file must be added to both `core/Makefile` and
`build_jni.py`. And it is not three repositories but four things to divide, because
`scripts-and-commands/` serves all of them and `commands-log.md` is a single
narrative across every phase that does not split at all.

**The observation that dissolves the question: consumers never see this
repository.** They install a jar from Maven Central or three packages from npm. The
C core already ships *inside* those artifacts as a bundled shared library and two
wasm modules, and is not a separate deliverable to anybody. The repository layout
is therefore not what makes the code public, and splitting it buys nothing on the
publishing path while costing the project its most valuable single test.

The same observation answers the related worry that the docs, specs and handoffs
are useless to consumers. They are, and it does not matter, because nobody clones
this to use the library. What a consumer reads is the npm package page, the Central
page and the javadoc. `docs/usage-java.md` and `docs/usage-javascript.md` are the
exception and are genuinely consumer facing, which is why the package READMEs link
to them.

The original recommendation, unchanged:

**Keep one repository, and stop describing it as three.** The determinism gate is
the evidence behind the central promise in spec section 9, that a server side
reconciliation and a browser preview produce the same report. A split means that
gate either moves to a fourth repository that checks out the other three at
pinned versions, or stops existing. Both are worse than a monorepo whose only
real cost is that `git clone` brings C, TypeScript and Java at once.

The three-repository idea came from the layout question in spec 13.0, before
there was anything to gate. There is now, and it changes the answer.

If you disagree and want the split, H3 is the honest version of it.

### H3. If splitting anyway

Do not split without doing H3.1 first. Everything else depends on the answer.

- [ ] **H3.1. Define the artifact the bindings consume.** Today they consume
      source. After a split they must consume something versioned. The choices:
      **(a)** `core` as a git submodule pinned by SHA in each binding repo, which
      keeps the source dependency and makes a cross-cutting change a three-commit
      dance; **(b)** `core` publishes prebuilt artifacts (the wasm modules, the
      header, the five native libraries, the generated fixtures) as a release,
      and the bindings depend on a version number. (b) is more work up front and
      is the only one that makes the repos genuinely independent.
- [ ] **H3.2. Decide where the determinism gate lives.** It needs all four
      producers at once. Realistically it lives in the `core` repository and
      pulls published binding artifacts, which means it tests the *last released*
      bindings rather than the current ones. Accept that or keep the monorepo.
- [ ] **H3.3. Publish the fixtures.** `core/fixtures/generated` is gitignored and
      regenerated by `gen_fixtures.py`. Both binding repos need identical
      fixtures for their tests to mean anything, so either publish them as a
      release asset with a checksum, or ship the generator and pin its seed.
- [ ] **H3.4. Split with history preserved.** Use `git filter-repo` (not the
      deprecated `filter-branch`) with `--path java/ --path-rename java/:` to
      produce a repository containing only that subtree with its commits intact.
      Repeat for `js/`. Keep the original repository untouched until both new
      ones are verified.
- [ ] **H3.5. Divide `scripts-and-commands/`.** It currently holds scripts for
      all three targets. `build_jni.py`, `measure_java_binding.py` and
      `run_phase6_checks.py`'s Java half go with `java/`; `gen_abi.mjs`,
      `copy-wasm.mjs`, `measure_binding.mjs`, `gen_stylesheet.mjs` and
      `wasm_smoke.mjs` go with `js/`; `check_determinism.mjs` goes wherever H3.2
      decided. **`commands-log.md` is a single narrative across all phases and
      does not divide.** Decide whether it stays with `core` or is copied.
- [ ] **H3.6. Replace every relative path from H1** with a reference to the
      artifact from H3.1. The `CORE_SOURCES` versus `CORE_SRC` assertion in
      `build_jni.py` cannot survive the split as written; replace it with a
      checksum of the consumed core artifact, so the guarantee it provides
      (the binding cannot silently run a different engine) survives in some form.
- [ ] **H3.7. Set up CI in each new repository** before archiving the monorepo,
      and confirm each is green independently.
- [ ] **H3.8. Do the split immediately after a release, never mid-phase**, so
      there is a known-good tagged point in all three histories that corresponds
      to the same engine.

---

## I. Suggested order

1. ~~**A** in full. Nothing is verifiable until the gate passes on this machine.~~
   **Done 2026-09-04.** It cost far less than budgeted, because the machine runs
   WSL2 rather than native Windows. A5 is the only item left open and it is
   parked, not outstanding: it applies to a native Windows build that nobody is
   doing.

   The real next step is the one this list does not contain, because
   `docs/PUBLISHING-PLAN.md` was written after it: **the five decisions in that
   plan's section 8**, which gate every task in F and G. They are a conversation,
   not work, and they block the largest branch of what is left.
2. **B**, against one real pair of files and one real query. This is the group
   where being wrong costs a customer, and it is roughly an afternoon.
3. **C1**, which is a decision and a paragraph, not an implementation.
4. **H2**, the repository decision. Deciding to keep one repository takes a
   sentence and stops the cost from rising further.
5. **F**, npm publishing. `csvdiff-core` and `csvdiff-view` are usable today and
   publishing them exercises the packaging paths while the code is fresh.
6. **G**, Maven publishing, which is gated on G2's CI matrix and is therefore the
   longer of the two.
7. **E10**, the demo app, which makes E1 through E8 assessable by hand.
8. **D** or **E**, in either order. Phase 7 is a throughput improvement on a
   parser that is already 2.4x above its floor; Phase 8 is the difference between
   a library and a product. If the next audience is users rather than consumers,
   do E first.
