#!/bin/sh
# Usage: ci_install_locked_apt.sh BUNDLE TRUSTED_RUNTIME_LOCK_SHA256
# The caller authenticates that digest and runs this in the exact declared base
# with --network=none. No Python/jq bootstrap and no online APT update are used.
set -eu
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
fail() { printf 'locked APT: %s\n' "$*" >&2; exit 1; }
[ "$#" -eq 2 ] || fail 'expected bundle and trusted runtime lock SHA-256'
[ "$(uname -s)" = Linux ] && [ "$(dpkg --print-architecture)" = amd64 ] || fail 'requires a linux/amd64 base'
input=$1
expected=$2
case "$expected" in *[!0-9a-f]*|'') fail 'invalid trusted lock digest' ;; esac
[ "${#expected}" -eq 64 ] || fail 'invalid trusted lock digest length'
[ -d "$input" ] && [ ! -L "$input" ] || fail 'bundle is not a real directory'
# Reject links/special files before copying. A second check on the private copy
# makes installation independent of later mutations to a host-mounted bundle.
[ -z "$(find "$input" ! -type f ! -type d -print)" ] || fail 'bundle contains links or special files'
[ -z "$(find "$input" -type f -links +1 -print)" ] || fail 'bundle contains hardlinks'
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cp -R "$input/." "$work/"
[ -z "$(find "$work" ! -type f ! -type d -print)" ] || fail 'copied bundle contains links or special files'
expected_directories=3
[ ! -d "$work/keys" ] || expected_directories=4
for directory in "$work"/*/; do
    case "${directory%/}" in "$work/debs"|"$work/lists"|"$work/keys") ;; *) fail 'unexpected bundle directory' ;; esac
done
[ "$(find "$work" -type d | wc -l)" -eq "$expected_directories" ] || fail 'unexpected nested bundle directory'
[ -f "$work/runtime.lock" ] && [ -f "$work/manifest.json" ] || fail 'missing manifests'
actual=$(sha256sum < "$work/runtime.lock")
[ "${actual%% *}" = "$expected" ] || fail 'runtime lock SHA-256 mismatch'
tab=$(printf '\t')
line=0
files=0
key_files=0
packages=0
requests=0
expected_files=0
set --
while IFS="$tab" read -r kind a b c d extra; do
    line=$((line + 1))
    case "$kind" in
        schema_version) [ "$line:$a:$b$c$d$extra" = '1:1:' ] || fail 'invalid runtime schema' ;;
        stage) [ "$line" -eq 2 ] && [ -n "$a" ] && [ -z "$b$c$d$extra" ] || fail 'invalid stage header' ;;
        platform) [ "$line:$a:$b$c$d$extra" = '3:linux/amd64:' ] || fail 'invalid runtime platform' ;;
        base_image) [ "$line" -eq 4 ] && [ -n "$a" ] && [ -z "$b$c$d$extra" ] || fail 'invalid base header' ;;
        file_count)
            [ "$line" -eq 5 ] && [ -z "$b$c$d$extra" ] || fail 'invalid file count header'
            case "$a" in *[!0-9]*|'') fail 'invalid file count' ;; esac
            expected_files=$a
            [ "$expected_files" -gt 0 ] && [ "$expected_files" -le 10000 ] || fail 'file count out of range'
            ;;
        requested)
            [ "$line" -gt 5 ] && [ -n "$a" ] && [ -z "$b$c$d$extra" ] || fail 'invalid package request'
            case "$a" in -*|*[!A-Za-z0-9_.:+~=-]*) fail 'unsafe package request' ;; esac
            requests=$((requests + 1))
            ;;
        prerequisite)
            # The caller replays this authenticated prefix. The installed base
            # inventory below must then match the current stage's captured state.
            [ "$line" -gt 5 ] && [ -n "$a" ] && [ -z "$d$extra" ] || fail 'invalid prerequisite record'
            case "$a" in *[!a-z0-9_.-]*) fail 'invalid prerequisite stage' ;; esac
            case "$b$c" in *[!0-9a-f]*) fail 'invalid prerequisite digests' ;; esac
            [ "${#b}" -eq 64 ] && [ "${#c}" -eq 64 ] || fail 'invalid prerequisite digest length'
            ;;
        file)
            [ "$line" -gt 5 ] && [ -z "$d$extra" ] || fail 'invalid file record'
            case "$a" in *[!0-9a-f]*|'') fail 'invalid file digest' ;; esac
            [ "${#a}" -eq 64 ] || fail 'invalid file digest length'
            case "$b" in *[!0-9]*|'') fail 'invalid file size' ;; esac
            case "$c" in *[!A-Za-z0-9_./:+%~=-]*|*/../*|*/./*|*//*|/*) fail 'unsafe file path' ;; esac
            case "$c" in
                sources.list|base-packages.tsv|index-targets.txt|lists/*|debs/*.deb) ;;
                keys/*.asc) key_files=$((key_files + 1)) ;;
                *) fail 'unexpected file path' ;;
            esac
            [ -f "$work/$c" ] && [ ! -L "$work/$c" ] || fail 'missing payload file'
            [ "$(wc -c < "$work/$c")" -eq "$b" ] || fail 'payload size mismatch'
            actual=$(sha256sum < "$work/$c")
            [ "${actual%% *}" = "$a" ] || fail 'payload SHA-256 mismatch'
            files=$((files + 1))
            ;;
        package)
            [ "$line" -gt 5 ] && [ -z "$extra" ] || fail 'invalid package record'
            case "$d" in debs/*.deb) ;; *) fail 'invalid package path' ;; esac
            case "$d" in */../*|*/./*|*//*|*[!A-Za-z0-9_./:+%~=-]*) fail 'unsafe package path' ;; esac
            [ "$(dpkg-deb -f "$work/$d" Package)" = "$a" ] || fail 'package name mismatch'
            [ "$(dpkg-deb -f "$work/$d" Version)" = "$b" ] || fail 'package version mismatch'
            [ "$(dpkg-deb -f "$work/$d" Architecture)" = "$c" ] || fail 'package architecture mismatch'
            case "$c" in
                amd64) set -- "$@" "$a:amd64=$b" ;;
                all) set -- "$@" "$a=$b" ;;
                *) fail 'unsupported package architecture' ;;
            esac
            packages=$((packages + 1))
            ;;
        *) fail 'unknown runtime lock record' ;;
    esac
done < "$work/runtime.lock"
[ "$files" -eq "$expected_files" ] && [ "$packages" -gt 0 ] && [ "$requests" -gt 0 ] || fail 'incomplete runtime lock'
[ "$(find "$work" -type f | wc -l)" -eq "$((files + 2))" ] || fail 'extra or missing bundle files'
if [ -d "$work/keys" ]; then
    [ "$key_files" -gt 0 ] || fail 'undeclared key directory'
else
    [ "$key_files" -eq 0 ] || fail 'missing key directory'
fi
dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\t${db:Status-Status}\n' > "$work/current-base.unsorted"
sort "$work/current-base.unsorted" > "$work/current-base.tsv"
cmp "$work/base-packages.tsv" "$work/current-base.tsv" || fail 'installed base package state differs from resolution'
# Relocate only the producer's generated Signed-By key selectors, never URLs,
# fingerprints, or arbitrary source options. All input bytes were checked above.
rewrite_sources() {
    while IFS= read -r source_line; do
        case "$source_line" in
            'deb [signed-by='*'] '*)
                scoped=${source_line#'deb [signed-by='}
                source_tail=${scoped#*'] '}
                remaining=${scoped%%'] '*}
                rewritten=
                scoped_paths=0
                scoped_fingerprints=0
                while :; do
                    selector=${remaining%%,*}
                    case "$selector" in
                        /inputs/keys/*.asc)
                            key_name=${selector#/inputs/keys/}
                            case "$key_name" in *[!A-Za-z0-9_.-]*|'') fail 'unsafe scoped key path' ;; esac
                            [ -f "$work/keys/$key_name" ] || fail 'missing scoped key'
                            relocated="$work/keys/$key_name"
                            scoped_paths=$((scoped_paths + 1))
                            ;;
                        *)
                            case "$selector" in *[!0-9A-F]*|'') fail 'invalid Signed-By fingerprint selector' ;; esac
                            [ "${#selector}" -eq 40 ] || fail 'invalid Signed-By fingerprint length'
                            relocated=$selector
                            scoped_fingerprints=$((scoped_fingerprints + 1))
                            ;;
                    esac
                    rewritten="${rewritten}${rewritten:+,}${relocated}"
                    [ "$remaining" != "$selector" ] || break
                    remaining=${remaining#*,}
                done
                [ "$scoped_paths" -gt 0 ] && [ "$scoped_fingerprints" -gt 0 ] || fail 'incomplete Signed-By policy'
                printf 'deb [signed-by=%s] %s\n' "$rewritten" "$source_tail"
                ;;
            'deb http://'*|'deb https://'*) printf '%s\n' "$source_line" ;;
            *) fail 'unexpected source options' ;;
        esac
    done
}
rewrite_sources < "$work/sources.list" > "$work/sources.runtime.list"
# Public scoped key files must remain readable by APT's sandbox user.
chmod 755 "$work"
if [ "$key_files" -gt 0 ]; then
    chmod 755 "$work/keys"
    chmod 644 "$work"/keys/*.asc
fi
apt-get -o "Dir::Etc::sourcelist=$work/sources.runtime.list" -o Dir::Etc::sourceparts=- \
    -o "Dir::State::lists=$work/lists" -o "Dir::Cache::archives=$work/debs" \
    -o APT::Get::AllowUnauthenticated=false -o Acquire::AllowInsecureRepositories=false \
    --no-download --reinstall --no-install-recommends -y install "$@"
# Successful APT exit is not enough: assert the exact installed version/state.
while IFS="$tab" read -r kind a b c d extra; do
    [ "$kind" = package ] || continue
    package=$a
    [ "$c" = all ] || package="$a:$c"
    actual=$(dpkg-query -W -f='${Version}\t${Architecture}\t${db:Status-Status}\n' "$package")
    expected=$(printf '%s\t%s\tinstalled' "$b" "$c")
    [ "$actual" = "$expected" ] || fail 'installed package does not match lock'
done < "$work/runtime.lock"
