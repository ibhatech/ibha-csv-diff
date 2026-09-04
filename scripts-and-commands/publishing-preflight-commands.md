# Publishing preflight: the commands run, and what they returned

Date: 2026-08-15. These are the checks behind section 1 of `docs/PUBLISHING-PLAN.md`.
Every one is read only except the two builds, which write to gitignored output
directories.

## Java

```bash
cd java && mvn -q -DskipTests package
```

**Failed.** One javadoc error and 100 warnings:

```
DiffSource.java:320: error: unexpected heading used: <H2>, compared to
                     implicit preceding heading: <H3>
```

Warnings are missing `@return` on `CsvDiffProvider` methods, missing `@param` on
`compare`, and no comment on the three `CsvDiffException` constructors. The
`target/ibha-csvdiff-java-0.1.0-javadoc.jar` present on disk is stale, dated before
the ingestion API work, so it is not evidence the javadoc jar builds.

## JavaScript

```bash
cd js && pnpm -r build
```

**Passed.** All three packages emit `dist`.

```bash
ls -la js/packages/core/wasm/
git ls-files js/packages/core/wasm js/packages/core/dist
```

Both `.wasm` modules present on disk (120,729 and 138,584 bytes), and **tracked by
git: no**. They come from `pnpm wasm`, which runs `make -C ../core wasm`.

## Registry and namespace availability

```bash
# npm scope, via the registry search API
curl -s 'https://registry.npmjs.org/-/v1/search?text=scope:ibhatech&size=20'
#   -> {"objects":[],"total":0}   the @ibhatech scope has no published packages

# Maven Central group id
curl -s 'https://search.maven.org/solrsearch/select?q=g:com.ibhatech&rows=20&wt=json'
#   -> "numFound":0               com.ibhatech is unclaimed

# the domain behind the group id, for Central's DNS TXT verification
dig +short ibhatech.com A ibhatech.com NS
#   -> 185.199.108-111.153 (GitHub Pages), ns11/21/31/41.zns-53.com|net
```

## Repository state

```bash
gh repo view ibhatech/ibha-csv-diff --json name,visibility,description,licenseInfo
#   -> {"visibility":"PRIVATE","licenseInfo":null,"description":""}

find . -maxdepth 2 -iname 'LICENSE*'   # -> nothing
ls .github                             # -> does not exist, there is no CI
```
