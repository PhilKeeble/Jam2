# Application updates

Jam2 checks the website for a newer stable version, gets that version's package
metadata from GitHub, and offers an update. Downloading and verification happen
asynchronously while the jam continues. Windows uses a temporary ZIP updater
after Jam2 closes; macOS opens Apple’s Installer for the verified PKG after
the user chooses Quit and Open Installer.

See [RELEASE.md](../RELEASE.md) for release preparation and publishing commands,
and [Gui.md](Gui.md#application-updates) for the user-facing controls.

## Versions and discovery

| Source | Purpose |
| --- | --- |
| Version compiled into `jam2` | The installed version used for update comparisons; also shown by `jam2 --version`. |
| Website `/version.txt` | The newest stable version advertised to clients. |
| Windows local `version.txt` | A readable version marker and a check on downloaded packages. It does not determine the running application's version. |
| GitHub `.zip.json` (Windows) / `.pkg.json` (macOS) | The selected archive's URL, size, hash and model requirement. |
| Windows packaged `package-manifest.json` | Platform, architectures, version and the files owned by the application package. |
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
   https://github.com/PhilKeeble/Jam2/releases/download/v0.1.2/Jam2-macos.pkg.json
   ```

5. Validate the metadata and show the version and download size for approval.
   The package is not downloaded during the check.

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

**Transition requirement:** the separate website repository must change its
Mac asset check from `Jam2-macos.zip` to `Jam2-macos.pkg` and deploy that change.
The Mac download link also needs the PKG name. That repository is not included
in this checkout. After deployment, the job should check the release tag and require both `Jam2-windows.zip` and
`Jam2-macos.pkg` to be uploaded, nonempty assets. It accepts a numeric version
tag with an optional `v`/`V` prefix and writes the bare version atomically to
`/var/www/jam2gether/version.txt`, served as `/version.txt`:

```text
0.1.2
```

This server check downloads release metadata only. It does not download the
ZIPs or inspect their contents, and currently does not check the adjacent
`.zip.json` / `.pkg.json` files. The publishing scripts perform the stronger readiness checks
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
`releases/latest/download/Jam2-windows.zip` and `Jam2-macos.pkg`. Jam2's updater
uses version-specific URLs so selecting an update is independent of later
changes to GitHub Latest. There is no website `release.json` endpoint.

## Required GitHub release structure

Build the Windows and macOS packages using the same explicit version. For
example:

```powershell
.\build-release.ps1 --version 0.1.2 --publish
```

```sh
QT_DIR="$HOME/Qt/6.x/macos" bash ./build-release.sh --version 0.1.2 \
  --sign-identity "Developer ID Application: YOUR_NAME (YOUR_TEAM_ID)" \
  --installer-sign-identity "Developer ID Installer: YOUR_NAME (YOUR_TEAM_ID)" \
  --notarize-profile "jam2-notary" --include-models --publish
```

Use a real universal Qt SDK prefix and the exact Keychain identity names.
The [configured release Mac commands](../RELEASE.md#macos-build-and-publish-commands-on-the-configured-release-mac)
provide the values validated on this machine. The same existing `jam2-notary`
profile can notarize both the app and PKG.

The application release tag must be `v0.1.2` for these clients. Its assets are:

```text
v0.1.2
  Jam2-windows.zip
  Jam2-windows.zip.json
  Jam2-macos.pkg
  Jam2-macos.pkg.json
```

Windows targets x86_64. The macOS package targets both arm64 and x86_64.
Scripts write the ZIP/PKG and metadata to `dist/`; without `--publish`, they remain
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

Application `.zip.json` / `.pkg.json` metadata has this shape. The sizes and hashes below are
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
manifest hash pins `models.json` in the Windows ZIP root or the Mac app’s
`Contents/Resources/` directory.
Do not hand-edit generated JSON or rezip an archive without regenerating its
metadata.

The Windows ZIP opens directly into its portable installation root and contains
`jam2.exe`, dependencies, private workers, notices, `version.txt`,
`package-manifest.json` and `models.json`. Its manifest declares owned files,
version, platform, architectures and hashes; extraction validates paths, links,
limits and hashes before installation. Weights and user data are excluded.

The macOS PKG installs only `/Applications/Jam2.app`. It contains Qt and TLS,
workers, ONNX Runtime, notices and `Contents/Resources/models.json` inside the
signed app. It never installs user data or models into `/Applications`.
The app is signed/notarized/stapled before PKG creation; the PKG receives its
own Developer ID Installer signature, notarization and stapled ticket.
Metadata hashes describe the final stapled PKG. The app's compiled version
identifies the installed version.

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
installed user model folder; if a later app needs different models, JamTaster checks
and downloads them on demand. Runtime downloads use release assets, not LFS.

## Preparing and applying an update

On Windows the writable root remains the folder containing `jam2.exe`.
For distributed macOS apps it is `~/Library/Application Support/Jam2/`, regardless
of where the running app lives. Source builds create `.jam2-development-data`
beside `release/Jam2.app` and use `release/` as their writable root. The PKG
never contains that marker or test data. App resources are read from the bundle; opening a JamJar
elsewhere preserves that project's own directory.

| Location under the writable root | Contents |
| --- | --- |
| `settings/preferences.ini` | User preferences. |
| `logs/`, `captures/`, `tracks/`, `songs/`, `cache/` | Runtime logs, recordings, unsaved/saved work and generated previews. |
| `components/jamtaster/models/` | Downloaded or manually extracted model weights/sidecars/notices. |
| `temp/jamtaster-models/` | Cancellable model ZIP staging. |
| `temp/updater/` | Temporary helper with its own Qt/TLS runtime, approved metadata and log. |
| `temp/updater.lock` | Concurrent update protection. |
| `update/package.zip` / `update/package.pkg` | Windows ZIP / macOS PKG and matching `.json` metadata. |
| `update/package/` | Windows unpacked replacement files. |
| `update/prepared.json` | Verified package marker. |
| `update/status.json`, `update/result.json` | Progress and outcome/handoff information. |

### macOS PKG workflow

1. The user approves the version and exact download size. The temporary helper
   reads app/Qt/TLS files from the installed bundle and stages them only in user
   data. It fetches the approved tagged PKG metadata again, rejecting changes
   since approval, and streams the package into `update/`.
2. Before marking it ready, the helper checks size/SHA-256, runs
   `pkgutil --check-signature` requiring a Developer ID Installer signature,
   and requires Gatekeeper's `spctl --assess --type install` acceptance.
3. **Quit and Open Installer** runs Jam2's ordinary save/close flow. Cancellation
   leaves Jam2 open and prepared data intact. **Later** and dismissing the dialog
   keep the package ready without opening Installer on an unrelated normal exit.
   On the next launch, the retained package is re-verified without redownloading it.
4. After the original Jam2 process exits, the helper re-verifies the package and
   opens it explicitly in Apple's Installer. Installer owns authorization,
   progress, cancellation and its success/error screen. The PKG requires all
   running `com.jam2.app` instances to close, installs only the app in Applications,
   and replaces obsolete bundle files. Jam2 stays closed.
5. Installer opening is recorded as `installer_opened`, not installation success.
   The PKG is retained. A subsequent launch with the expected compiled version
   confirms installation and cleans staging/helper files after Installer closes. Launching an old copy
   explains that the user should complete Installer and open Jam2 in Applications.

A new download also preserves an existing PKG while Apple’s Installer is open,
including when the package was opened manually.

User settings, models and projects are outside the installer payload and survive
updates. Installer never creates a user Library folder as root or launches Jam2
as root. Old ZIP-only Mac updaters require one manual PKG install to transition.

### Windows ZIP workflow

Preparation verifies the tagged archive's size/digest, manifest, file hashes,
version, platform and architecture, then exits while Jam2 keeps playing.
**Restart and install** or **Install on exit** launches the prepared helper only
when Jam2 actually closes; cancelling a save prompt preserves the queued update.
The helper re-verifies staged files, preflights locks, replaces package-owned
files and removes obsolete owned files. The original direct replacement has no
backups/rollback. Interrupted replacement can be repaired by extracting a full
release while preserving user data. Only the explicit restart action reopens Jam2.

## Cancellation, cleanup and testing

Closing the progress popup leaves preparation running. **Cancel update** stops
preparation and removes its temporary files; closing Jam2 before preparation
finishes also cancels it. Closing a ready popup defers installation; **Discard
update** removes the queued update. If Apple’s Installer is already open,
cancellation preserves its PKG and reports that Installer must be quit before
discarding it. Private helper files are still cleaned.

There is one reusable `update/` staging folder. A new download clears stale
staging rather than accumulating versions. The lock prevents clearing an active
installation. On Windows, after success the helper removes the staged package and ZIP; the
next GUI launch removes the remaining result/staging files and temporary
runtime. macOS retains its PKG until the expected compiled version is launched.
With Windows **Install on exit**, that final cleanup waits until the next
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
- **Missing updater:** the downloaded package was built before the helper was
  included; rebuild/upload it with the current scripts.
- **Metadata/hash mismatch:** assets were replaced during approval/download,
  JSON does not match the package, or the download is damaged. Check again after
  correcting/reuploading the release.
- **TLS initialization failed:** required Qt TLS plugins/dependencies are
  missing. Windows needs `tls/qschannelbackend.dll` in the installed package and
  the staged helper runtime. On macOS, source checkouts should run `update.sh`
  with the same `QT_DIR` used to compile, or leave it unset for Homebrew Qt.
  This checks an actual TLS backend load and installs/repairs Homebrew Qt only
  when necessary. Distributed Mac PKGs must carry their TLS plugins in
  `Jam2.app/Contents/PlugIns/tls`; installing developer dependencies on a
  recipient's machine is not a replacement for a self-contained release.
- **Cannot replace a file:** another Jam2 instance/worker still holds it, or the
  installation folder is not writable. Close it and retry.

### macOS file permissions and older data

Distributed apps never write default data into the bundle or its parent. Application Support is
created by the logged-in user, so running a downloaded app from a read-only
translocation location does not redirect logs or workspaces there. App resources
and workers stay in the bundle. See [Apple's App Translocation Notes](https://developer.apple.com/forums/thread/724969).

Adjacent/profile preferences are copied when the new INI is absent; old default
log/capture destinations are redirected, including stale App Translocation
paths. Custom paths are preserved. Old recordings/projects/models remain in the
old extraction folder; open those JamJars there or copy desired folders to the
new data root without overwriting newer work. The new installed app cannot
infer an arbitrary previous ZIP extraction location.

To install a previously downloaded model ZIP manually, extract its `models/`
folder into `~/Library/Application Support/Jam2/components/jamtaster/`.
For source-build testing, extract into `release/components/jamtaster/` instead.
The in-app model setup checks hashes against the manifest inside the app before
using existing weights or downloading replacements. Downloads/extraction never
write inside the signed bundle.

On Windows, the installed local `version.txt` is currently rewritten with an LF newline
after installation. A Windows package may contain CRLF, so that one file's raw
hash can differ after a successful update. Its value is unchanged, package
version checks trim whitespace, and future update detection uses the compiled
version. This does not prevent later updates.

Only stable update discovery is currently implemented. Separate bleeding-channel
selection is future work. Native tests cover PKG payload generation, signing/notarization failure paths,
Installer handoff policy, model ZIPs and user-data paths. Both Developer ID
certificates and the existing `jam2-notary` profile have produced a signed,
notarized PKG accepted by Gatekeeper. Validate the actual Installer workflow
using the [installed-package test steps](../RELEASE.md#daily-macos-development-and-installed-package-testing)
before distribution.

Implementation owners: `app/updater/` (check, preparation, helper and package
installation), `app/gui/UpdateDialog.*` / `UpdateUi.*` (interface),
`tools/release/` (packaging/publishing), and the website repository's `deploy.yml`
(server version synchronization).
