# p2f for PS4

A PS4 homebrew app that downloads files from a
[peer-to-file](https://github.com/murilopereirame/peer-to-file) server.
It is a C port of [p2f-lib](https://github.com/murilopereirame/p2f-lib).

The wire protocol and the crypto are the same as in p2f-lib:

- Bearer token authentication.
- Resumable downloads. A stopped file keeps its bytes. The next try continues from that byte (HTTP `Range`).
- AES-256-CTR decrypt during the download. The key is ECDH-wrapped (P-256, HKDF-SHA256, AES-256-GCM).
- SHA-256 check of each finished file.
- HTTP or HTTPS (TLS 1.2). HTTPS checks the server certificate.

## Layout

| Path | Contents |
|---|---|
| `core/` | The portable C library (`p2f.h`). No PS4 code. |
| `ps4/` | The PS4 app: screen, controller, download queue, PS4 platform layer. |
| `tools/` | Desktop test tool, mock server, test script. |
| `Makefile` | PS4 build (`.pkg`). |
| `Makefile.host` | Desktop build of the library and `p2f-cli`. |

## Requirements

- A jailbroken PS4 with a homebrew enabler (for example GoldHEN).
- The [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (tested with v0.5.2).
- `clang` and `ld.lld` (LLVM). On macOS: `brew install llvm`.
- A peer-to-file server that the PS4 can reach over HTTP or HTTPS.

## Build

```sh
scripts/fetch_deps.sh                       # mbedTLS, cJSON, font8x8
export OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
make                                        # -> IV0000-PTOF00001_00-P2FDOWNLOADER000.pkg
```

`PkgTool.Core` (from the toolchain) needs OpenSSL **1.1**. If the build stops with
"No usable version of libssl was found", put `libssl.so.1.1` and `libcrypto.so.1.1`
on `LD_LIBRARY_PATH` (Linux) or use the toolchain's Docker image.

## Desktop tests

```sh
make -f Makefile.host
tools/test_host.sh
```

The tests start a mock server (Node 20+) with the same crypto as the real server.
They check: folder and file download, listing, bad token, missing path,
stop and resume, connection loss and resume, resume from an odd byte offset,
a server that ignores `Range`, a bad local file, a checksum failure, and skip of complete files.

The HTTPS tests (Python `cryptography` makes the test certificates) check:
RSA and ECDSA certificates, a private CA, a self-signed certificate,
an IP address in the URL, resume over TLS, and the rejection of an unknown CA,
a wrong host name and an expired certificate.

The DNS tests use a mock DNS server: A record, CNAME chain, NXDOMAIN,
no A record, and a server that does not answer.

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
- To update the CA list: `scripts/gen_ca_bundle.py <cacert.pem>` (see the script header).

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

## Library API

See `core/p2f.h`. The main calls:

```c
p2f_client *c;
p2f_client_new(&c, "https://files.example.com", "p2f_...", 0);
p2f_client_set_ca_file(c, "/data/p2f/ca.pem");   /* optional */
p2f_set_dns_server("1.1.1.1");                   /* optional, global */
p2f_download_opts o = { on_progress, user, 1 /* verify */, 60000 /* idle ms */ };
int rc = p2f_download_path(c, "movies/Film.2024", "/data/p2f/downloads", &o, NULL, 0);
if (p2f_is_retryable(rc)) { /* call again: it resumes */ }
p2f_client_free(c);
```

## Differences from p2f-lib

- An empty remote file makes an empty local file. (p2f-lib does not make the file.)
- `P2F_ERR_SHORT` replaces the "file is short" exception. `p2f_is_retryable()` tells which errors to retry.
- A network error during the body becomes `P2F_ERR_SHORT`, so the retry resumes.

## Licenses

- This code: MIT (same as p2f-lib).
- mbedTLS: Apache-2.0. cJSON: MIT. font8x8: public domain.
- `core/ca_bundle.c`: Mozilla CA list (via certifi), MPL-2.0.
- `libc.prx`, `libSceFios2.prx` and `right.sprx` come from the OpenOrbis toolchain at build time. They are not in this repository.
