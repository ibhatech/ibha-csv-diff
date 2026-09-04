# Publishing plan: Maven Central and npm

Date: 2026-08-15
Status: **proposal, for review.** Nothing in here is implemented. Phase 7 (the SIMD
parser) is on hold behind it, per Manas.

Scope: publish `com.ibhatech:ibha-csvdiff-java` to Maven Central through the
Sonatype Central Portal, and `@ibhatech/csvdiff-core`, `-view` and `-react` to the
public npm registry. The C core and CLI are not published as artifacts of their own;
they ship inside these two, as a bundled `.so`/`.dylib`/`.dll` and as two `.wasm`
modules.

---

## 0. Three decisions that block everything, and they are yours

**Answered 2026-09-04 by Manas, except 0.3.** Recorded here rather than in a
separate file, so the question and its answer stay next to each other.

| Decision | Answer |
|---|---|
| 0.1 Public distribution, or private | **Public.** Open source, published to Maven Central and npmjs.com |
| 0.2 Which license | **Apache-2.0** |
| 0.3 How many platforms the jar carries | **Two: `linux-x86_64` and `darwin-aarch64`.** See the note below |
| Repository public (section 8 item 4) | **Yes** |
| `developers` block (section 8 item 5) | **Manas Marthi, manas@ibhatech.com.** In the pom |

**On 0.3, answered 2026-09-04: two platforms, not one and not five.**
`linux-x86_64` and `darwin-aarch64`, which are the machines Manas actually
runs and can therefore actually test. This is neither option A nor option B
below, and it is better than both for a 0.1.0: option A ships something no
Mac can load, and option B spends a day or two on Windows and two more targets
that nobody has asked for and nobody can verify by hand.

**It makes one existing bug live.** `NativeLibrary`'s failure message names all
five platforms as bundled. Shipping two while claiming five sends a consumer
looking for a packaging fault that is really an unsupported platform. The
message and both READMEs have to state exactly what is in the jar, and that is
part of J3 rather than something to leave until someone reports it.

The CI matrix shrinks to two runners, `ubuntu-latest` and `macos-14`, which is
hours rather than the one to two days option B was costed at. Adding
`linux-aarch64`, `darwin-x86_64` or `windows-x86_64` later is one more job each
and needs no change to the loader, which already looks for all five.

Two consequences of 0.1 and 0.2 worth stating once, because they are the part that
cannot be walked back:

1. **Apache-2.0 lets anyone, including a competitor, use and redistribute the
   engine commercially.** That is the trade accepted by choosing it. It is the
   right trade if the engine is infrastructure, and the wrong one if it is a
   product differentiator.
2. **Making the repository public publishes group B of
   `specs/03-remaining-tasks.md` along with it**, including its statement that
   five assumptions are unconfirmed and that each "fails silently: the diff
   returns a wrong answer that looks like a right answer". That sentence is
   honest and should not be deleted. **It is the reason group B runs before the
   repository is flipped public**, not a reason to hide it: confirming the five
   turns the most alarming public paragraph in the repository into a settled
   decision.

The original text of each decision follows, kept because the reasoning is what
will be questioned later.

Nothing else in this plan can be executed until these are answered. They are first
because two of them change the shape of the plan rather than a step inside it.

### 0.1 Public distribution, or private

**Maven Central and npmjs.com are public and effectively permanent.** Central never
deletes a published version. npm allows unpublish only within 72 hours and only when
nothing depends on the package. Publishing to either is a one way door for that
version number.

Today `github.com/ibhatech/ibha-csv-diff` is **private** and carries **no LICENSE
file**. Central additionally requires a **sources jar**, so the Java source is
published whether or not the repository is, and a placeholder sources jar is a poor
look on Central and blocks IDE navigation for every consumer.

So the real question is not "how do we publish" but "is this library public
software". Three coherent answers:

| Answer | Targets | What changes |
|---|---|---|
| **Open source** (recommended if the goal is adoption) | Maven Central + npmjs.com | Pick a license, make the repo public, this plan runs as written |
| **Free binaries, closed source** | Maven Central is a poor fit; npm tolerates it | Sources jar becomes a placeholder, npm packages ship `dist` only (already true). Expect friction |
| **Customers only** | GitHub Packages, Artifactory, or AWS CodeArtifact, plus npm private packages | Different plan. Same build work, different distribution and auth. Roughly the same effort |

**Recommendation: open source under Apache-2.0**, and publish to both public
registries. The argument in spec section 9 (one engine so the browser and the server
cannot disagree) only pays off if consumers can actually take the dependency, and a
JNI plus wasm library that people cannot read is a hard sell. If any part of the
engine is meant to be commercial, say so now, because it inverts the plan.

### 0.2 Which license

Needed as: a `LICENSE` file at the repo root, a `<licenses>` block in `pom.xml`, and
an SPDX id in all three `package.json` files (they currently say `UNLICENSED`, which
npm renders as "this package may not be used").

**Recommendation: Apache-2.0.** It carries an explicit patent grant, which MIT does
not, and it is the default expectation for a JVM library. MIT is fine if you prefer
brevity. If it is to be proprietary, we need the actual license text from you and
section 0.1 resolves to the third row.

### 0.3 How many platforms the Java jar carries

The jar bundles one native library per platform under `native/<os>-<arch>/` and
extracts it on first use. `NativeLibrary` already looks for five and its error
message already promises five: `linux-x86_64`, `linux-aarch64`, `darwin-aarch64`,
`darwin-x86_64`, `windows-x86_64`. **This machine builds one of them.** A jar
published today would fail on every non Linux consumer with an error message that
claims those platforms are bundled.

| Option | Effort | Result |
|---|---|---|
| **A. Ship linux-x86_64 only** | hours | Correct if the README and the error message say so. Fine for an internal server side consumer, embarrassing on Central |
| **B. Build five in GitHub Actions, ship one fat jar** (recommended) | 1 to 2 days, Windows is most of it | What a consumer expects. Jar grows to roughly 1.5 MB |
| **C. One classifier artifact per platform** | 1 day plus a loader change | Smaller downloads, more consumer friction, and `NativeLibrary` would need to change |

**Recommendation: B**, with the caveat that `build_jni.py` is written for a POSIX
toolchain and Windows is the one target that needs real work (MSVC does not take the
warning flag set, and the library name and export rules differ). If Windows turns
out to be more than a day, drop it from 0.1.0 and ship four, documented, rather than
delay.

---

## 1. Where the two artifacts actually stand

Verified today, not recalled. The exact commands are in
`scripts-and-commands/publishing-preflight-commands.md`.

### 1.1 Java

| Check | State |
|---|---|
| `mvn -DskipTests package` | **Fails.** Javadoc error, see below |
| Sources jar | Configured and produced |
| Javadoc jar | Configured, currently **cannot build** |
| Required Central POM fields | **Missing `url`, `licenses`, `developers`, `scm`** |
| GPG signing | Not configured, no key on this machine |
| Central Portal plugin | Not present |
| Native coverage | linux-x86_64 only, see 0.3 |
| `com.ibhatech` on Central | Unclaimed (search returns 0 artifacts) |
| `ibhatech.com` | Resolves, GitHub Pages A records, so DNS TXT verification is available |

The javadoc failure is one hard error plus 100 warnings:

```
DiffSource.java:320: error: unexpected heading used: <H2>, compared to
                     implicit preceding heading: <H3>
100 warnings   (missing @return on interface methods, missing @param, no comment
                on the three CsvDiffException constructors)
```

The error is a one line fix (`<h2>` to `<h3>`). The warnings do not block, but 100
of them on a library whose javadoc is its documentation is worth an hour. Note the
javadoc jar sitting in `target/` is stale, dated before the ingestion API work; it
is not evidence that this passes.

**Good news on coordinates:** `com.ibhatech` is free and you control `ibhatech.com`,
so the groupId does not have to change. No `io.github.*` fallback is needed.

### 1.2 JavaScript

| Check | State |
|---|---|
| `pnpm -r build` | **Passes**, all three packages emit `dist` |
| `.wasm` modules | Present on disk, **not committed**, built by `pnpm wasm` |
| `license` field | `UNLICENSED` in all three |
| `repository`, `homepage`, `bugs`, `author` | **Missing in all three** |
| `publishConfig.access` | Missing; scoped packages default to restricted |
| `workspace:*` dependencies | Present in view and react, rewritten by `pnpm publish` at pack time |
| `files` whitelist | Correct: `dist`, `wasm`, `styles`, `README.md` |
| `@ibhatech` npm scope | **Unregistered**, no packages published under it |
| Module format | ESM only, no CJS build. Node 20+ and every bundler are fine; a `require()` consumer is not |

**The one dangerous failure mode:** `js/packages/core/wasm/*.wasm` are gitignored
build outputs. A publish from a clean checkout without running `pnpm wasm` first
produces a package that installs cleanly, type checks cleanly, and throws at the
first `compare()` because the engine is missing. This gets a `prepack` guard, not a
line in a runbook.

### 1.3 Neither side

No `.github/` directory. There is no CI at all today. Everything currently runs from
`scripts-and-commands/run_phase6_checks.py` on this machine.

---

## 2. Versions and coordinates

| Artifact | Coordinate | Version |
|---|---|---|
| Java | `com.ibhatech:ibha-csvdiff-java` | `0.1.0` |
| JS engine | `@ibhatech/csvdiff-core` | `0.1.0` |
| Headless view | `@ibhatech/csvdiff-view` | `0.1.0` |
| React components | `@ibhatech/csvdiff-react` | `0.1.0` |

Rules I propose we hold to:

1. **The three npm packages version in lockstep**, and view and react depend on core
   with a caret range at the released version. They are one product cut into three
   for tree shaking, not three products.
2. **The Java artifact versions independently of npm.** They share a C core but not a
   release cadence, and a JNI fix should not bump a browser package.
3. **0.x means the API can break on a minor bump.** Say it in both READMEs. Both
   bindings are three phases old and the SIMD parser has not landed yet.
4. **The engine version is separate from the binding version** and is already
   reported by `CsvDiff.engineVersion()`. A bug report needs both, so both go in the
   issue template.
5. **Every published version is permanent.** Rehearsals happen against
   `pnpm pack` tarballs and a deferred Central deployment, never against a real
   version number. If 0.1.0 goes out broken, the fix is 0.1.1, forever.

---

## 3. The Java pipeline

### Phase J1: make the build releasable (local, no accounts needed)

1. Fix `DiffSource.java:320` and the javadoc warnings. Gate it: add
   `<doclint>all</doclint>` and `<failOnWarnings>true</failOnWarnings>` to the
   javadoc plugin so this cannot regress silently the way it just did.
2. Add the Central required POM metadata: `url`, `licenses`, `developers`, `scm`.
3. Add `LICENSE` and `NOTICE` at the repo root, and have the jar plugin include them
   under `META-INF/`.
4. Add a `release` profile carrying `maven-gpg-plugin` and
   `org.sonatype.central:central-publishing-maven-plugin`, with `autoPublish=false`
   so the first deployment lands as a validated but unpublished bundle we can inspect
   and drop.
5. Prove it: `mvn -P release -DskipTests verify` produces jar, sources, javadoc and
   an `.asc` for each, and `run_phase6_checks.py --require-java` still passes.

### Phase J2: accounts and keys (yours, not mine)

1. Create a Central Portal account at `central.sonatype.com`.
2. Register the `com.ibhatech` namespace, then add the verification TXT record to
   `ibhatech.com` DNS. Verification is usually minutes.
3. Generate a signing key, publish the public half to `keyserver.ubuntu.com`, and
   keep the private half and its passphrase somewhere durable. **Losing this key is
   the single hardest thing here to recover from**, since every future release must
   be signed. It belongs in a password manager, not on this laptop only.
4. Generate a Portal user token and put it in `~/.m2/settings.xml` under server id
   `central`. Never in the repository.

### Phase J3: the native matrix (the real work)

1. Add `.github/workflows/native.yml`: five jobs, one per target, each running
   `build_jni.py` and uploading `native/<os>-<arch>/` as an artifact.
   - `ubuntu-latest` for linux-x86_64
   - `ubuntu-24.04-arm` for linux-aarch64
   - `macos-14` for darwin-aarch64, `macos-13` for darwin-x86_64
   - `windows-latest` for windows-x86_64
2. Teach `build_jni.py` about Windows: library naming, the export mechanism, and a
   warning flag set MSVC accepts, or use clang on Windows and keep one flag set. This
   is the step most likely to overrun. Timebox it to a day.
3. Verify each platform runs the suite, not just links. A `.dylib` that loads and
   then disagrees about a diff is worse than one that fails to load, and the
   determinism gate is what catches it.

### Phase J4: release

1. A tag driven workflow collects the five native artifacts into `java/target/native/`
   and runs `mvn -P release deploy` once, from one runner.
2. Inspect the deferred bundle in the Portal: five natives present, sources and
   javadoc present, signatures valid, POM complete.
3. Publish. Central sync makes it resolvable within roughly 30 minutes.
4. **Smoke test from outside the repo**: a scratch Maven project in an empty
   directory with one dependency, running a real diff, on Linux and macOS at minimum.
   The point is to catch what only a consumer sees: a missing resource, a bad
   `Automatic-Module-Name`, a native extraction failure under a read only tmpdir.

---

## 4. The npm pipeline

### Phase N1: metadata and guards (local)

1. Create the `@ibhatech` organisation on npmjs.com. It is free for public packages
   and the scope is currently unclaimed. **Do this early**, since the name is the one
   thing another user could take.
2. In all three `package.json` files: real `license`, `repository` with a `directory`
   field, `homepage`, `bugs`, `author`, and `publishConfig: { "access": "public" }`.
3. Add `scripts/check-wasm.mjs` and wire it as `prepack` in the core package: fail if
   either `.wasm` is missing, and warn if either is older than the newest file in
   `core/src`. This is the guard for 1.2's failure mode.
4. Copy `LICENSE` into each package directory.
5. Decide and document what a consumer gets for the wasm asset. The default resolves
   `../wasm/<name>` relative to the module, which Vite, webpack and Node all handle,
   and `configure({ wasmUrl })` is the escape hatch for a CDN. This is worth a
   dedicated README section per registry, because it is the first thing that breaks
   in an unusual bundler setup.

### Phase N2: rehearsal

1. `pnpm -r build && pnpm -r test`, then `pnpm pack` in each package.
2. `tar tzf` each tarball and read the file list. Expect `dist`, both `.wasm`,
   `styles` for view, README and LICENSE. Expect **no** `src`, no `.test.ts`, no
   `node_modules`.
3. Install the three tarballs into two scratch projects: a Vite plus React app (proves
   bundler resolution of the wasm URL and the React peer range), and a plain Node
   script (proves the `exports` map and the worker entry outside a bundler).
4. Only after both work does anything get published.

### Phase N3: publish

1. Publish in dependency order: core, then view, then react. `pnpm publish` rewrites
   `workspace:*` to the released version at pack time; **do not use `npm publish`
   here**, it does not.
2. Verify each on the registry, and reinstall from the registry rather than from the
   tarball into the same two scratch projects.

### Phase N4: automate (optional, recommended after the first manual release)

Move publishing to a tag driven GitHub Actions workflow using **npm trusted
publishing with OIDC**: no `NPM_TOKEN` in the repository, and provenance attestations
generated automatically. It needs `id-token: write` and npm CLI 11.5.1 or newer.

**Caveat tied to 0.1:** provenance attestations require a public repository. If the
repo stays private, this phase becomes a granular access token in a GitHub secret and
no provenance.

---

## 5. Documentation

Two usage guides are written and land with this plan:

- `docs/usage-java.md`
- `docs/usage-javascript.md`

They are task shaped (compare two files, compare a result set, stream a report,
render a table) rather than a class listing, and they document the traps that cost
real time: the `source()` present only when changed rule, the header row count
contract, `changesOnly` never dropping a finding row, disposing a handle.

For publishing, three more things are needed:

1. The npm package README **is** the registry landing page. Each package README gets
   an install line, a 10 line example, and a link to the usage guide.
2. Central shows the POM description and the javadoc. `package-info.java` for
   `com.ibhatech.csvdiff` would make the javadoc landing page useful rather than an
   alphabetical class list.
3. A root `README.md`. There is not one today, and it is what a Central or npm
   visitor clicks through to.

---

## 6. Sequencing and rough effort

| Phase | Depends on | Effort | Who |
|---|---|---|---|
| 0. The three decisions | nothing | a conversation | **Manas** |
| J1 javadoc, POM, license | 0.1, 0.2 | half a day | me |
| N1 metadata, wasm guard, scope | 0.1, 0.2 | half a day | me, plus the org creation |
| Docs: usage guides, READMEs, root README | 0.2 | delivered with this plan, plus half a day of READMEs | me |
| J2 Portal account, namespace, GPG | 0.1 | an hour of yours, then waiting | **Manas** |
| J3 native matrix CI | 0.3 = B | 1 to 2 days, Windows is the risk | me |
| N2 rehearsal | N1 | half a day | me |
| J4 Java release | J1, J2, J3 | half a day | me, publish button yours |
| N3 npm release | N1, N2 | an hour | me |
| N4 trusted publishing | N3, public repo | half a day | me |

**Shortest honest path to something published:** decisions, then N1 plus N2 plus N3.
The npm side can ship in about two days because there is no native matrix in its way.
The Java side is gated on J3 unless you take option A in 0.3, which would make it
about the same two days for a Linux only artifact.

---

## 7. Risks, and what we do about each

| Risk | Mitigation |
|---|---|
| A broken 0.1.0 is permanent | Rehearse against tarballs and a deferred Central bundle. Never test with a real version |
| npm package ships without the wasm | `prepack` guard, plus the tarball file list read by eye in N2 |
| Native library missing for a consumer's platform | Ship what we build and say exactly what that is, in the README **and** in `NativeLibrary`'s error message, which currently overpromises |
| GPG key lost | Password manager, off this machine, before J4 |
| Scope or namespace taken | Both verified free today. Claim both in the first week |
| JDK 24+ native access warning surprises consumers | Already documented in the Java README. Repeat it in the usage guide and the Central description |
| ESM only breaks a CJS consumer | Document it. A CJS build is real work and nobody has asked yet |
| Published artifacts drift from the gate | Release workflow runs `run_phase6_checks.py --require-java` before it deploys anything |

---

## 8. What I need from you to start

**Three of the five are answered, 2026-09-04.** See the table at the head of
section 0.

1. ~~**0.1** public or private distribution.~~ **Public.**
2. ~~**0.2** the license.~~ **Apache-2.0.**
3. **0.3** Linux only now, or the five platform matrix first. **Still open.** This
   is the one that changes how long J3 takes, and it is now the only decision
   gating the Java pipeline.
4. ~~Whether to make the GitHub repository public.~~ **Yes**, after group B of
   `specs/03-remaining-tasks.md`, per the sequencing note in section 0.
5. The `developers` block content for the POM: name and email as you want them
   permanently visible. **Still open**, and it blocks nothing until J1 is ready to
   commit a pom.

J1, N1 and the README work can now start on 1, 2 and 4 alone. J3 waits on 3, and
the final pom commit waits on 5.

---

## 9. Out of scope, deliberately

- **Publishing the C core or the CLI** as artifacts. They ship inside the two
  bindings. A CLI binary via GitHub Releases is a reasonable later ask.
- **Phase 7, the SIMD parser.** On hold behind this, per Manas. Nothing here touches
  `core/src`, so the two do not conflict, and the SIMD work will inherit whatever CI
  this plan builds.
- **A CJS build, a Vue or Svelte binding, an FFM backend.** All previously deferred,
  all still deferred.
