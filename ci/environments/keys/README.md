# Ubuntu toolchain PPA public keys

These are public verification keys, not credentials. Preserve their original
armor bytes; the input manifests bind the SHA-256 of each file. Producers must
scope these keys to the declared toolchain PPA source with `Signed-By`, never
add them to the global Ubuntu trust store or fetch an unpinned replacement.

| File | Primary fingerprint | SHA-256 |
| --- | --- | --- |
| `toolchain-current.asc` | `C8EC952E2A0E1FBDC5090F6A2C277A0A352154E5` | `aa6927526a6e522ddeac8b82920f72098e558677255f86ce7278aa68d9df7e47` |
| `toolchain-legacy.asc` | `60C317803A41BA51845E371A1E9377A2BA9EF27F` | `2646b417592e0f5216bc6242268a72163123c99da0ba2350fcb0fadaaf8197ec` |

The current fingerprint was obtained on 2026-09-23 from the
[official Launchpad archive API](https://api.launchpad.net/1.0/~ubuntu-toolchain-r/+archive/ubuntu/test).
The legacy key corresponds to the `1E9377A2BA9EF27F` identifier already used by
this project's Focal recipe. The armored bytes came from Ubuntu's keyserver:

- [Current key](https://keyserver.ubuntu.com/pks/lookup?op=get&search=0xC8EC952E2A0E1FBDC5090F6A2C277A0A352154E5)
- [Legacy key](https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x60C317803A41BA51845E371A1E9377A2BA9EF27F)

The Focal `InRelease` at the
[official PPA endpoint](https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu/dists/focal/InRelease)
was dual-signed. Both signatures were verified with `gpgv` in an isolated,
network-free Ubuntu container, using a temporary dearmored keyring. Both
`VALIDSIG` fingerprints matched the table and the complete verification exited
successfully. This verifies the observed signing-key transition; it is not a
claim that every future PPA response or the complete Focal compiler environment
has been qualified.
