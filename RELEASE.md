# Release preparation

Release plan; completed work is noted below.

## Implementation order

1. **Portable storage (implemented):** preferences use `settings/preferences.ini`, VST3 scan results use `temp/`, and storage uses the binary folder on Windows or the folder enclosing `Jam2.app` on macOS without ancestor searching. Existing profile preferences migrate when the portable INI is absent. Models retain their existing location; update staging is implemented with the updater steps below.
2. **Packaging and metadata (implemented; Mac execution still needs validation):** use the release scripts below to stamp a version, compile and produce the application ZIP and optional separate model ZIP. This establishes the file/model manifests before updater and downloader work.
3. **Publishing (implemented; ready for live testing) and server flow:** use `--publish` to create/reuse releases and upload each OS ZIP with clobber. New app releases remain draft until both platforms and their referenced model releases are ready; then publish and remove previous stable app releases. Update/test the server job that selects stable, bleeding and model releases separately and generates `/release.json` from GitHub metadata.
4. **Models on demand:** implement JamTaster's download prompt, progress, cancellation and verification using the pinned `models.json`. Until then, extract the model ZIP manually into the Jam2 folder. Preserve installed models across app updates.
5. **Updater helper:** first test installation from an already downloaded package. Implement clean process-exit handling, direct replacement of owned files, helper replacement and failure reporting. Preserve settings, models and user content; no backups or rollback.
6. **Update interface:** connect asynchronous release checks, user approval, verified downloads and progress to the helper. Add **Restart and install** and **Install when Jam2 closes**, with one reusable staging folder.
7. **End-to-end validation:** exercise older-to-newer releases, model reuse, cancellation, failed/corrupt downloads, locked files and interrupted installation. Check both operating systems and Mac architectures, staging cleanup and writes outside the Jam2 folder. Add relevant native CTests throughout the work; run optimized Release validation before distribution.
8. **First updater-enabled release:** build/upload both OS ZIPs for the chosen version and verify the server manifest. The supplied version is authoritative; differing commits do not block publishing. Existing users install this release manually once; later releases use the updater.

Community/documentation work can proceed alongside these steps. Keep the connection experiments separate from the updater implementation.

## Version tracking and updates

- Generate the packaged and compiled versions from the release script's `--version` override, or CMake's development default `0.1.0` when omitted. The override does not edit source files or persist to later ordinary builds. The planned updater checks `https://jam2gether.com/release.json` and compares major/minor/patch numerically; unavailable metadata must not block startup.
- Manifest format (byte counts and hashes are placeholders):

  ```json
  {
    "schema": 1,
    "version": "0.2.0",
    "tag": "v0.2.0",
    "packages": {
      "windows": {
        "url": "https://github.com/PhilKeeble/Jam2/releases/download/v0.2.0/Jam2-windows.zip",
        "bytes": 12345678,
        "sha256": "<64 hexadecimal characters>"
      },
      "macos": {
        "url": "https://github.com/PhilKeeble/Jam2/releases/download/v0.2.0/Jam2-macos.zip",
        "bytes": 12345678,
        "sha256": "<64 hexadecimal characters>"
      }
    }
  }
  ```

- Keep exactly one Windows ZIP and one macOS ZIP, using the filenames above. Publish the manifest atomically only when both packages are uploaded with known sizes/hashes; preserve the previous manifest on failure. Add `/release.json` to the server path allowlist; keep the website's existing download filenames.
- The planned updater downloads the exact advertised package after approval, with progress/cancellation, and verifies size, hash, platform and architecture. `package-manifest.json` records package-owned files/hashes (or link targets); its own filename is reserved and not self-hashed. `models.json` pins the model release and weight hashes for that installed app, even when the app is older than the server's release.
- Use one `update/` folder inside Jam2 for downloads, staging and the running helper copy. Clear stale contents before each new download, never while installation is running; only a fully verified download is ready to install. Clean up after success.
- Offer **Restart and install** or **Install when Jam2 closes**. Finish saves and stop workers before installation. Use direct replacement without backups or rollback; report failures, with manual reinstallation as the recovery path.
- Ship a private C++ updater in `components/updater` on Windows and inside the Mac bundle. Run its copy from `update/` so it can replace the installed helper too. Preserve user data and models, and record the version only after successful replacement.
- Replace owned files on Windows and the whole Mac bundle. Build Windows for Intel/AMD x64 only; Windows Arm support is out of scope. Target a universal Intel/Apple Silicon Mac app, including Qt, workers, updater and ONNX Runtime. Verify HTTPS dependencies and Mac packaging/signing; advertise only validated architectures.

## Release build scripts

Run on each platform from the same source revision and with the same version:

```powershell
.\build-release.ps1 --version 0.8.1 --include-models
```

```sh
bash ./build-release.sh --version 0.8.1 --include-models
```

- Scripts reuse `compile.cmd`/`compile.sh` and stop on failure. They build optimized Release and use CMake for packaging; no new packaging runtime is required.
- `dist/Jam2-windows.zip` or `dist/Jam2-macos.zip` includes only declared application files, dependencies, helpers, notices, `version.txt`, `package-manifest.json` and `models.json`. Model weights and user data are always excluded. `jam2 --version` and Windows/Mac version metadata carry the same version.
- `--include-models` additionally creates `dist/Jam2-models.zip`, with weights/sidecars under `components/jamtaster/models/`, `models.json` and model notices. It reuses an existing ZIP when its model tag/manifest match and its size/hash verify, even when the app version changes. Without the flag, an existing model ZIP is left untouched. Package creation verifies each weight's size/hash against its source sidecar and rejects LFS pointers.
- Each ZIP has an adjacent `.zip.json` recording its channel, version, URL, byte count, SHA-256, upload tag and required model tag/manifest hash. Generated JSON uses the same line endings on both platforms so manifest hashes agree. App tags are `v<version>`; model tags are `models-<content hash>` derived from the required weight list, independent of app version and shared across platforms. Reuse that model release until weights change.
- Mac builds require universal dependencies, including a universal Qt installation selected with `QT_DIR`, and both vendored ONNX dylibs. The script combines ONNX architectures, bundles dependencies, verifies architectures/linkage and uses `ditto` to preserve bundle links/permissions. No certificate is required for local ad-hoc-signed packages. Later use `--sign-identity "Developer ID Application: ..."`; notarization remains a separate step.
- Without `--publish`, scripts only create local artifacts. The server assembles `/release.json` from GitHub metadata after upload. Only the application/model requirement manifests belong inside the app ZIP; finished archive hashes remain outside it.

### Publishing

Install GitHub CLI and run `gh auth login` on each build machine. Add `--publish`
with an explicit version when ready to upload, for example:

```powershell
.\build-release.ps1 --version 0.8.1 --include-models --publish
```

```sh
bash ./build-release.sh --version 0.8.1 --publish
```

- Publishing always targets `github.com/PhilKeeble/Jam2`. Missing tags/releases are created automatically; existing tags, notes and the other OS assets are not rewritten. New application tags target local HEAD when available; model tags use GitHub's default branch and their release title is **JamTaster Models**. There is no clean-tree or commit-matching gate; version increments and release readiness are the maintainer's decision.
- Each run uploads its OS ZIP and `.zip.json` with `--clobber`. `--include-models` also uploads the model ZIP, its `.zip.json` and `models.json` to the content-derived model release, publishing it without taking GitHub's repository-wide Latest badge. A complete, verified published release for the required model manifest is reused without uploading models again.
- A new application release stays draft until both OS ZIPs and matching metadata are uploaded and their referenced model ZIPs/manifests are available. The final machine publishes it as stable/Latest. Replacing assets in an existing published release requires its model dependencies to be available first.
- After successful publication, remove other published stable application releases identified by version tags and app ZIPs. Keep Git tags, model releases, drafts, unrelated releases and prereleases/bleeding releases. No cleanup occurs after an upload failure or while the new release is incomplete.
- GitHub releases must be mutable for clobber. Replacing an asset deletes its previous upload first; a failed upload can leave it absent until retried. Reusing the same version does not notify already-installed clients; the server must refresh changed asset sizes/hashes.
- The server should discover latest stable, bleeding and models within their categories rather than relying on GitHub's single Latest badge. Bleeding publishing flags are not implemented yet.

## Everything stays in the Jam2 folder

- The extracted folder is the portable root. On macOS, keep mutable data beside `Jam2.app`, outside its signed bundle.
- Store preferences as the single `settings/preferences.ini` file, with `settings/` beside `tracks/` and the other data folders.
- Store VST3 scan results under `temp/` in this root and remove them after scanning.
- Keep Jam2-managed defaults for preferences, models, recordings, songs, tracks, logs, caches, downloads, staging and helper copies inside this root. Report unwritable locations instead of falling back to profile or system temporary folders. Qt's own file-dialog state, user-selected recording/log/export destinations, external JamJar workspaces, and explicit CLI/debug outputs are accepted exceptions.
- Derive paths from the installation, independently of the open project or working directory; prevent extracted archive paths and filesystem links escaping the intended destination.
- Models live at `components/jamtaster/models/` under the portable root on both platforms, outside the signed Mac bundle and separate from `update/`. Use uploaded model release assets rather than LFS downloads or source archives. Mark model releases as not latest (`make_latest: false`) so application discovery and existing latest-download links remain correct.
- Each application pins the model tag, URL, unpacked size and file hashes in `models.json`; the planned downloader obtains the ZIP's compressed size/digest from that exact GitHub release. This lets Windows and Mac packages share a model release without rebuilding its archive. The prompt/downloader is still planned. Native workers/ONNX Runtime remain in app packages.
- Test install, analysis, downloads, updates, cancellation and failures for writes outside the root, preservation of models/user data, and reuse of `update/`. Deleting the folder should remove all Jam2-managed files and preferences.

## Community and documentation

- Create a Discord group for support, bug reports and testing; publish its invite.
- Create YouTube videos for installation/audio setup, starting/joining jams, connection troubleshooting and JamTaster.
- Improve READMEs: downloads, supported platforms, portable storage/removal, updates, first-jam instructions, troubleshooting and community/video links.

## Connection experiments

- Test server-side endpoint coordination and simultaneous UDP hole punching; audio stays direct, with no relay/TURN path.
- Test replacing TCP control with authenticated UDP, covering reliability, ordering, retries, duplicates and bounded unauthenticated work. Aim to avoid manual router rules; measure remaining NAT/firewall limitations.
- Record connection success, setup time and failure reasons across NATs/CGNAT/firewalls. Extend native tests, including exactly-four-peer impairment workflows; validate optimized Release builds before distribution.

## Look into current management of files

cache is building in release and looks like not being cleaneed
tracks vs songs, both look similar, no clear split there 
maybe re-arrange files in local to have clearer splits 

make bleeding branch and allow for those builds to grab bleeding versions for updates? 
