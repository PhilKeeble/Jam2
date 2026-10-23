# Application updates

Jam2 checks the website for a newer stable version, gets that version's package
metadata from GitHub, and offers an update. Downloading and verification happen
asynchronously while the jam continues. A temporary updater helper replaces the
application only after Jam2 closes.

See [RELEASE.md](../RELEASE.md) for release preparation and publishing commands,
and [Gui.md](Gui.md#application-updates) for the user-facing controls.

## Versions and discovery

| Source | Purpose |
| --- | --- |
| Version compiled into `jam2` | The installed version used for update comparisons; also shown by `jam2 --version`. |
| Website `/version.txt` | The newest stable version advertised to clients. |
| Local `version.txt` | A readable version marker and a check on downloaded packages. It does not determine the running application's version. |
| GitHub `.zip.json` | The selected archive's URL, size, hash and model requirement. |
| Packaged `package-manifest.json` | Platform, architectures, version and the files owned by the application package. |
| Packaged `models.json` | The model release and model hashes required by this build. |

CMake generates `BuildVersion.hpp` from its project version. A release script's
`--version 0.1.2` sets `JAM2_RELEASE_VERSION` for that build and stamps the binary,
platform version information and package files consistently. This does not edit
the source version. A later ordinary compile uses the development default in
the root `CMakeLists.txt`, currently `0.1.0`.

The normal GUI schedules a background check three seconds after startup. The
header's **Updates** button also allows a manual check. There is no periodic
polling during the jam. The check performs:

1. GET `https://jam2gether.com/version.txt`.
2. Require HTTP 200, trim whitespace and validate `major.minor.patch`.
3. Compare the three numeric components with the compiled version. Equal or
   older advertised versions produce **Jam2 is up to date**.
4. If newer, GET the current OS's small metadata file from the exact version tag:

   ```text
   https://github.com/PhilKeeble/Jam2/releases/download/v0.1.2/Jam2-windows.zip.json
   https://github.com/PhilKeeble/Jam2/releases/download/v0.1.2/Jam2-macos.zip.json
   ```

5. Validate the metadata and show the version and download size for approval.
   The ZIP is not downloaded during the check.

Requests bypass the client cache and use HTTPS with redirects that cannot
downgrade to HTTP. Startup check failures stay quiet; opening **Updates** shows
the error and permits retry. Checking never needs GitHub authentication.

Changing the installed `version.txt` cannot make the executable appear older.
Likewise, replacing a GitHub ZIP under the same version does not notify clients
already running that version. Increment the release version when existing users
should receive an update.

## How the website learns the version

The website's `deploy.yml` installs the Debian systemd service
`jam2gether-version-sync.service` and timer `jam2gether-version-sync.timer`.
The default schedule is **daily at 03:00 UTC**, with an initial run during
deployment and catch-up after a missed run.

The service GETs:

```text
https://api.github.com/repos/PhilKeeble/Jam2/releases/latest
```

It checks the published release's tag and requires both `Jam2-windows.zip` and
`Jam2-macos.zip` to be uploaded, nonempty assets. It accepts a numeric version
tag with an optional `v`/`V` prefix and writes the bare version atomically to
`/var/www/jam2gether/version.txt`, served as `/version.txt`:

```text
0.1.2
```

This server check downloads release metadata only. It does not download the
ZIPs or inspect their contents, and currently does not check the adjacent
`.zip.json` files. The publishing scripts perform the stronger readiness checks
described below. Invalid/incomplete releases and API failures preserve the
previous advertised version. Before the first valid release, the endpoint may
return 404.

The nightly schedule means update notifications can lag behind publication.
On the server, refresh and inspect it with:

```sh
sudo systemctl start jam2gether-version-sync.service
sudo journalctl -u jam2gether-version-sync.service
sudo cat /var/lib/jam2gether/version-sync-status.json
```

Website download buttons separately point at GitHub's
`releases/latest/download/Jam2-windows.zip` and `Jam2-macos.zip`. Jam2's updater
uses version-specific URLs so selecting an update is independent of later
changes to GitHub Latest. There is no website `release.json` endpoint.

## Required GitHub release structure

Build the Windows and macOS packages using the same explicit version. For
example:

```powershell
.\build-release.ps1 --version 0.1.2 --publish
```

```sh
bash ./build-release.sh --version 0.1.2 --publish
```

The application release tag must be `v0.1.2` for these clients. Its assets are:

```text
v0.1.2
  Jam2-windows.zip
  Jam2-windows.zip.json
  Jam2-macos.zip
  Jam2-macos.zip.json
```

Windows targets x86_64. The macOS package targets both arm64 and x86_64.
Scripts write the ZIPs and metadata to `dist/`; without `--publish`, they remain
local artifacts. Publishing uses GitHub CLI authentication on the build machine.

Each machine uploads its OS pair with `--clobber`, leaving the other OS assets
and existing release notes intact. Missing tags/releases are created
automatically. A new application release remains draft until both OS pairs and
their referenced model releases pass the publishing checks. The final upload
publishes it as stable/Latest and removes previous published stable application
releases while retaining their Git tags. Model releases, drafts and prereleases
are retained. Commit equality is not a publishing requirement; the maintainer
controls version increments and readiness.

The publisher compares uploaded asset sizes and GitHub SHA-256 digests with the
small JSON assets it fetches. It does not need to download the ZIPs again to
perform those checks. When clobbering an already-published release, retry a
failed upload before advertising it: replacement removes the old asset first.

### Archive metadata and contents

An application `.zip.json` has this shape. The sizes and hashes below are
illustrative; the scripts calculate the real values:

```json
{
  "schema": 1,
  "channel": "stable",
  "version": "0.1.2",
  "tag": "v0.1.2",
  "asset": "Jam2-windows.zip",
  "bytes": 54000000,
  "sha256": "<64 lowercase hexadecimal characters>",
  "url": "https://github.com/PhilKeeble/Jam2/releases/download/v0.1.2/Jam2-windows.zip",
  "models_tag": "models-<content hash>",
  "models_manifest_sha256": "<SHA-256 of the packaged models.json>"
}
```

Jam2 validates the schema, stable channel, version, tag, fixed OS filename and
exact expected GitHub download URL, plus the size/hash fields. The model
manifest hash pins the `models.json` bytes carried inside the application ZIP.
Do not hand-edit generated JSON or rezip an archive without regenerating its
metadata.

Each application ZIP opens directly into the portable installation root,
without an extra enclosing directory. It contains:

- `version.txt`, `package-manifest.json` and `models.json` at its root.
- Windows: `jam2.exe`, Qt/runtime DLLs, platform/TLS plugins, component workers
  and `components/updater/jam2-updater.exe`.
- macOS: `Jam2.app`, including dependencies, workers and
  `Contents/Helpers/jam2-updater` inside the app bundle.
- The declared licenses and notices.

`package-manifest.json` uses schema 1, a version, `platform` (`windows` or
`macos`), `architectures`, `models_manifest: "models.json"` and a `files` list.
Regular file entries have `path`, `bytes` and `sha256`; supported Mac symlinks
record `path` and `link`. Paths are relative to the installation root. The
manifest does not hash itself. The final ZIP's size and hash belong in its
external `.zip.json`, not inside the ZIP.

Application ZIPs exclude model weights and user data. A release without the
helper cannot be an updater target. Users starting from a build without an
updater must install an updater-enabled package manually once.

## Models are a separate download

Add `--include-models` when model release assets also need packaging/uploading.
The content-derived `models-<hash>` tag is independent of the application version;
its visible release title is **JamTaster Models**. It contains:

```text
Jam2-models.zip
Jam2-models.zip.json
models.json
```

The ZIP contains only `models/`, with weights, sidecars and notices. It extracts
under `components/jamtaster/` on both operating systems. Unchanged verified
model ZIPs/releases are reused. The model release is published without taking
the repository-wide Latest badge, so the website continues to discover the
application release.

The installed application's `models.json` pins its required model tag, archive
URL and per-model sizes/hashes. Opening JamTaster checks the installed models
and offers an asynchronous download if needed, with approval, size, progress,
cancellation and errors. Metadata comes from that pinned model release rather
than whichever release currently has Latest. Application updates preserve the
installed model folder; if a later app needs different models, JamTaster checks
and downloads them on demand. Runtime downloads use release assets, not LFS.

## Preparing and applying an update

The portable root is the executable's folder on Windows, or the folder enclosing
`Jam2.app` on macOS. Opening a JamJar workspace elsewhere does not change this
root. Update files stay inside it:

| Location | Contents |
| --- | --- |
| `temp/updater/` | Temporary helper, its Qt runtime/TLS dependencies and approved metadata. |
| `temp/updater.lock` | Lock preventing concurrent updater operations for this installation. |
| `update/package.zip.json` | Metadata for the selected download. |
| `update/package.zip` | Downloaded application archive. |
| `update/package/` | Unpacked, verified replacement files. |
| `update/prepared.json` | Marker identifying the prepared version and manifest hash. |
| `update/status.json` / `update/result.json` | Progress and final success/error information. |

The GUI flow has two separate helper runs:

1. **Prepare while Jam2 runs.** After approval, a worker copies the installed
   helper and its Qt dependencies into `temp/updater/`. The temporary helper
   GETs the tagged metadata again and compares its URL, size and hashes with
   the approved values before fetching the ZIP. A changed release requires a
   fresh check. The helper streams the download into `update/`, checks its
   size/SHA-256, unpacks with miniz and verifies the manifest, file hashes,
   version, platform and architectures. Mac code signatures are also checked.
   Unsafe paths, undeclared files and links escaping the intended root are
   rejected. Once ready, it writes the prepared marker and exits.
2. **Install after Jam2 closes.** A ready update defaults to installation on
   normal exit, without reopening Jam2. **Restart and install** requests the
   normal save/close flow and asks the helper to reopen the updated app.
   Cancelling a save/close prompt leaves Jam2 running and the update queued for
   a later normal exit. The installer launches from the existing temporary
   helper copy and waits for the original Jam2 process to exit. It re-verifies
   staged files before replacement. The wait is limited to two minutes; a
   deferred ready update has no helper waiting in the background.

Windows preflights destination file locks, replaces package-owned files and
removes obsolete files listed by the previous package manifest. macOS replaces
the whole `Jam2.app` bundle and updates owned files beside it. The ownership
manifest and local version marker are committed after replacement. Because the
helper runs from `temp/updater/`, it can replace its own installed binary and Qt
dependencies too; the next update uses the newly installed helper.

Qt Core/Network handles updater HTTPS and miniz handles extraction in process.
The updater does not invoke curl, tar, PowerShell or other OS command-line tools.
Platform code handles process checks, Windows file locks and Mac signatures.

Settings (`settings/preferences.ini`), models (`components/jamtaster/models/`),
tracks, songs and other user content are preserved. The implementation performs
direct replacement without backups or rollback. Installation errors are
reported on the next launch; if replacement was interrupted, manually extract a
complete release to repair application files while retaining user data.

## Cancellation, cleanup and testing

Closing the progress popup leaves preparation running. **Cancel update** stops
preparation and removes its temporary files; closing Jam2 before preparation
finishes also cancels it. Closing a ready popup defers installation; **Discard
update** removes the queued update.

There is one reusable `update/` staging folder. A new download clears stale
staging rather than accumulating versions. The lock prevents clearing an active
installation. After success the helper removes the staged package and ZIP; the
next GUI launch removes the remaining result/staging files and temporary
runtime. With **Install on exit**, that final cleanup waits until the next
launch. Failures retain diagnostic/download files in `update/` until retry;
startup cleanup removes the completed temporary helper runtime when possible.
Installation logs use `temp/updater/updater.log`; download preparation reports
progress directly to the GUI and `update/status.json`.

For an end-to-end GUI test, publish an updater-enabled target, refresh the
server version, and run a build with a genuinely older compiled version. Check
`jam2 --version`; editing local `version.txt` is insufficient. Normal development
builds currently use `0.1.0`, which can test a published `0.1.1` target.

Common explanations for unexpected results:

- **Up to date:** the server version is equal/older, the running binary was
  built with the new version already, or the nightly server refresh has not run.
- **Missing updater:** the downloaded ZIP was built before the helper was
  included; rebuild/upload it with the current scripts.
- **Metadata/hash mismatch:** assets were replaced during approval/download,
  JSON does not match the ZIP, or the download is damaged. Check again after
  correcting/reuploading the release.
- **TLS initialization failed:** required Qt TLS plugins/dependencies are
  missing. Windows needs `tls/qschannelbackend.dll` in the installed package and
  the staged helper runtime.
- **Cannot replace a file:** another Jam2 instance/worker still holds it, or the
  installation folder is not writable. Close it and retry.

The installed local `version.txt` is currently rewritten with an LF newline
after installation. A Windows package may contain CRLF, so that one file's raw
hash can differ after a successful update. Its value is unchanged, package
version checks trim whitespace, and future update detection uses the compiled
version. This does not prevent later updates.

Only stable update discovery is currently implemented. Separate bleeding-channel
selection is future work. Windows installation has been exercised manually;
macOS installation and both Mac architectures still need end-to-end validation.

Implementation owners: `app/updater/` (check, preparation, helper and package
installation), `app/gui/UpdateDialog.*` / `UpdateUi.*` (interface),
`tools/release/` (packaging/publishing), and the website repository's `deploy.yml`
(server version synchronization).
