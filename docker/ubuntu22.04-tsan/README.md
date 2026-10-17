# Source-built Qt for the Linux TSan environment

This recipe builds Qt 5.15.19 with Clang 14 ThreadSanitizer instrumentation.
It applies the repository's Qt object-publication patch before compiling Qt.
The final image retains the source inputs at:

```text
/usr/share/klogg-ci/qt-sources/
```

The sources travel inside the same image as the Qt libraries and tools. Their
availability does not depend on a separate, expiring workflow artifact.

## Retained contents

- `archives/` contains the four exact, original compressed source archives for
  qtbase, qtsvg, qttools, and qttranslations. Their SHA-256 values are the fixed
  arguments in `recipe/Dockerfile`. They are not repacked or modified archives.
- `patches/fix_qt5_qobject_tsan_publication.patch` is the exact patch applied to
  qtbase by this recipe. The original archives plus this patch describe the
  repository-maintained source modification.
- `recipe/` contains this Dockerfile and its online APT retry and ELF runtime
  verification helpers. Locked builds also retain the exact offline APT
  installer that was used; online builds do not invent that file.
- `licenses/<module>/` contains unchanged regular top-level `LICENSE*` files
  copied from each verified archive's extracted source directory. The original
  archives also retain per-file notices and bundled third-party license texts.
- `apt/` contains the actual bootstrap, source-dependency, Qt-builder, and
  runtime APT manifests in locked builds. These record package versions and
  input identities; they are not replacement package payloads or source offers.
  Legacy online builds have no invented APT manifests here.

The final image keeps the original compressed archives, not another expanded
copy of the multi-gigabyte build tree.

## Read-only source proof

Use the verifier from the matching, trusted klogg checkout:

```sh
python3 /path/to/klogg/scripts/verify_tsan_qt_sources.py \
  --sources /usr/share/klogg-ci/qt-sources \
  --repo-root /path/to/klogg \
  --require-locked
```

Qualification uses `--require-locked`. For an explicitly selected legacy online
build, omit that flag. The verifier compares archive pins and repository-owned
files with the checkout, compares license copies with members read directly
from the compressed archives, and emits a JSON proof. It does not fetch,
extract, patch, build, or execute source code. Do not use an untrusted copy of
the retained recipe as the trusted checkout for this comparison.

This is a technical source-input check. The broader image qualification also
checks the instrumented Qt artifacts and their runtime dependency closure.

## Reconstructing the modified Qt sources

Work in a separate directory, leaving the retained archives unchanged. After
checking their hashes, extract the four archives there and apply:

```sh
patch --batch --forward --fuzz=0 \
  --directory="qtbase-everywhere-src-5.15.19" \
  --strip=1 < /usr/share/klogg-ci/qt-sources/patches/fix_qt5_qobject_tsan_publication.patch
```

The extracted module directory names use `everywhere-src`, even though the
archive filenames use `everywhere-opensource-src`.

Follow the exact shared `qtbase-builder` and `qt-builder` commands in the
included Dockerfile. They select `clang-14`/`clang++-14`, configure Qt with
`-sanitize thread`, install to `/opt/qt5-tsan`, and build qtsvg, the required
Linguist command-line tools, and translations. Keep the included instrumentation
and ELF-closure checks when reproducing the environment.

For a full Docker rebuild, reconstruct the recipe context using the included
recipe files and patch. Its explicit default, `KLOGG_APT_STAGE=online`, downloads
from the pinned Ubuntu snapshot and hash-checks the Qt/CMake downloads. For the
qualified `locked` path, supply the four verified APT bundles, their lock hashes,
the retained Qt archives under `inputs/qt/`, and the hash-pinned CMake installer.
All qualified build steps run without network access. The exact input paths and
argument names are specified by the Dockerfile.

The retained Qt source directory is not a complete offline package cache: APT
`.deb` closures and the CMake installer must be supplied separately for that full
locked-image rebuild. The APT manifests document the actual dependency inputs.
No bit-for-bit rebuild claim is made merely by distributing these files.

## License information

Consult the actual upstream license files and source notices retained above for
the applicable terms, including notices for bundled third-party components.
Preserve those notices and the repository patch/build instructions when passing
on the modified Qt sources. Selecting the recipe's open-source configuration is
not, by itself, a determination of every redistribution obligation.

This source payload concerns the Qt libraries and Qt tools built by this recipe.
It does not claim to provide the corresponding sources or satisfy license
obligations for every other Ubuntu package, compiler, or tool in the CI image.
