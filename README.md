# p2f for PS4

A PS4 homebrew app that downloads files from a
[peer-to-file](https://github.com/murilopereirame/peer-to-file) server.
The download client is [p2f-lib](https://github.com/murilopereirame/p2f-lib),
the same C library that peer-to-file's apps use (as WebAssembly). This
repository is the PS4 app around it: screen, controller, queue.

From p2f-lib:

- Bearer token authentication.
- Resumable downloads. A stopped file keeps its bytes. The next try continues from that byte (HTTP `Range`).
- AES-256-CTR decrypt during the download. The key is ECDH-wrapped (P-256, HKDF-SHA256, AES-256-GCM).
- SHA-256 check of each finished file.
- HTTP or HTTPS (TLS 1.2). HTTPS checks the server certificate.
- Large files: the server reads and hashes a file once before it can send
  it, which takes minutes for a big file. The app shows "Server is preparing
  the file" and waits (Square stops it) instead of failing.

## Layout

| Path | Contents |
|---|---|
| `ps4/` | The PS4 app: screen, controller, download queue, and `platform_ps4.c` (the p2f-lib platform hooks: random, DNS, sleep, clock). |
| `p2f-lib/` | p2f-lib as a git submodule, pinned to one commit. `make` builds its PS4 library from source. |
| `scripts/fetch_deps.sh` | Checks out the submodule, fetches p2f-lib's own sources (mbedTLS, cJSON) and the font. |
| `Makefile` | PS4 build (`.pkg`). |
| `.github/workflows/build-pkg.yml` | CI: builds the `.pkg` on merges to `main` and on demand. |

## Requirements

- A jailbroken PS4 with a homebrew enabler (for example GoldHEN).
- The [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (tested with v0.5.2).
- `clang`, `ld.lld` and `llvm-ar` (LLVM). On macOS: `brew install llvm`.
- A peer-to-file server that the PS4 can reach over HTTP or HTTPS.

## Build

```sh
git clone --recurse-submodules https://github.com/murilopereirame/p2ps.git
cd p2ps
scripts/fetch_deps.sh                       # submodule + mbedTLS, cJSON, font8x8
export OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
make                                        # -> IV0000-PTOF00001_00-P2FDOWNLOADER000.pkg
```

`make` builds `p2f-lib/build/ps4/libp2f.a` first. To try a p2f-lib change,
edit or check out another commit inside `p2f-lib/` and run `make` again.

### CI

`.github/workflows/build-pkg.yml` builds the `.pkg` on every merge to `main`,
on every published GitHub release, and when started by hand (Actions ->
"Build PS4 pkg" -> Run workflow; you can name a p2f-lib branch to build
against). The `.pkg` is always a workflow artifact. Builds from `main` also
update the [`latest` pre-release](../../releases/tag/latest), and a release
build attaches `p2f-ps4-<tag>.pkg` to its release.

`.github/workflows/bump-version.yml` ("Bump version", run by hand) is
separate: it only raises `VERSION` in the `Makefile` (the app version the PS4
shows, `XX.YY`) and commits that to `main`. Pick `minor` (1.00 -> 1.01),
`major` (1.07 -> 2.00), or type an exact version. Locally:
`scripts/bump_version.sh minor`.

To release: run "Bump version", then publish a release with a tag such as
`v1.01`.

`PkgTool.Core` (from the toolchain) needs OpenSSL **1.1**. If the build stops with
"No usable version of libssl was found", put `libssl.so.1.1` and `libcrypto.so.1.1`
on `LD_LIBRARY_PATH` (Linux) or use the toolchain's Docker image.

## Tests

The client tests (desktop build of the library against a mock server: resume,
connection loss, checksum, HTTPS, DNS, large-file preparing) are in p2f-lib:
`make test` there.

## Use on the PS4

1. Install the `.pkg` and start the app once. The app writes `/data/p2f/config.ini`.
2. Edit the file (for example over FTP):
   ```ini
   url   = https://files.example.com     # or http://10.0.0.1:8000
   token = p2f_...
   dest  = /data/p2f/downloads
   # ca  = /data/p2f/ca.pem              # optional, see "HTTPS" below
   # dns = 1.1.1.1                       # optional, see "Host names" below
   ```
   Make the token on the server with `node src/server/cli.ts add-token <user> <name>`.
3. Start the app again.

| Button | Action |
|---|---|
| Up / Down | Move the cursor (hold to repeat). |
| Left / Right | Move one page. |
| X | Open a folder, or add a file to the queue. |
| Triangle | Add the current folder to the queue. |
| O | Go to the parent folder. |
| Square | Stop the current download (the bytes stay on disk). |
| Options | Refresh the list. |

The app retries a failed download up to 8 times and resumes each time.
To resume a stopped download, add it to the queue again.
The log is in `/data/p2f/p2f.log`.

### HTTPS

- The app trusts the Mozilla CA list (118 roots, built in). A certificate from
  Let's Encrypt or another public CA works with no extra setup.
- For a private CA or a self-signed certificate, copy the CA (or the
  self-signed certificate) as PEM to the PS4 and set `ca = /data/p2f/ca.pem`.
  Then the app trusts only that file.
- The certificate must name the host in `url` (DNS name or IP address).
- The PS4 clock must be correct. A wrong date makes valid certificates fail.
- TLS 1.2 only. Nginx, Caddy, Traefik and Cloudflare accept TLS 1.2 by default.
- The CA list is part of p2f-lib (`scripts/gen_ca_bundle.py` there).

### Host names (DNS)

The app finds the server address in this order:

1. An IP address in `url` needs no lookup.
2. If `dns` is set, the app asks only that DNS server.
3. Else the PS4 system resolver.
4. If that fails, the app asks the DNS servers in the PS4 network settings directly.

The result is kept for 5 minutes. If no step works, the error shows the reason
for each step (for example `NXDOMAIN` or `no answer`).
If the name exists only in your LAN or VPN, set `dns` to the DNS server that knows it.

### Notes

- Keep the app in the foreground. The PS4 suspends apps in the background.
- A USB drive is at `/mnt/usb0`. FAT32 cannot keep files larger than 4 GB. Use exFAT.
- Text on screen uses an 8x8 font with Latin-1 characters. Other characters show as `?`.

## Library

See [p2f-lib](https://github.com/murilopereirame/p2f-lib) (`include/p2f.h`).
To move to a new p2f-lib version:

```sh
git -C p2f-lib fetch --tags && git -C p2f-lib checkout v0.4.0
git add p2f-lib && git commit -m "Use p2f-lib 0.4.0"
```

## Licenses

- This code: MIT (same as p2f-lib).
- p2f-lib bundles mbedTLS (Apache-2.0), cJSON (MIT) and the Mozilla CA list
  (MPL-2.0); see its `THIRD_PARTY.md`. font8x8: public domain.
- `libc.prx`, `libSceFios2.prx` and `right.sprx` come from the OpenOrbis toolchain at build time. They are not in this repository.
