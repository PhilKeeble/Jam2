# Release preparation

Release plan; completed work is noted below.

## Implementation order

1. **Portable storage (implemented):** preferences use `settings/preferences.ini`, VST3 scan results use `temp/`, and storage uses the binary folder on Windows or the folder enclosing `Jam2.app` on macOS without ancestor searching. Existing profile preferences migrate when the portable INI is absent. Models retain their existing location; update staging is implemented with the updater steps below.
2. **Packaging and metadata (implemented; Mac execution still needs validation):** use the release scripts below to stamp a version, compile and produce the application ZIP and optional separate model ZIP. This establishes the file/model manifests before updater and downloader work.
3. **Publishing (implemented; ready for live testing) and server flow:** use `--publish` to create/reuse releases and upload each OS ZIP with clobber. New app releases remain draft until both platforms and their referenced model releases are ready; then publish and remove previous stable app releases. Keep the existing server job that writes the published stable version to `/version.txt`; package metadata is fetched directly from the matching GitHub release when a newer version is detected.
4. **Models on demand (implemented):** opening JamTaster checks the pinned models and offers their download with size, approval, progress, cancellation and verification. Network requests are asynchronous; hashing/extraction run on a low-priority worker. Failures offer Retry; closing the dialog cancels setup and returns to the jam. Downloads/extraction use one reusable `temp/jamtaster-models/` folder inside Jam2, cleaned on completion/cancellation/failure. Preserve installed models across app updates.
5. **Updater helper (implemented; macOS execution still needs validation):** Qt Core/Network handles HTTPS and miniz handles ZIP extraction inside the helper. Run a temporary helper/runtime copy from `temp/updater/`; download/unpack and verify in `update/`, then exit the preparation helper. Launch a separate installation pass only when Jam2 actually closes. Includes helper replacement, failure reporting and next-launch cleanup. Preserve settings, models and user content; no backups or rollback.
6. **Update interface (implemented; ready for manual testing):** background startup checks and the header's **Updates** button use the styled update dialog. Show version/size before approval, progress through download/verification, cancellation, **Restart and install** and **Install on exit**. Closing a progress dialog leaves preparation running; closing a ready dialog defers installation without reopening Jam2. Cancelling the normal save/close prompt keeps the verified update ready for the next normal exit. Errors allow retry without stopping the jam.
7. **End-to-end validation:** exercise older-to-newer releases, model reuse, cancellation, failed/corrupt downloads, locked files and interrupted installation. Check both operating systems and Mac architectures, staging cleanup and writes outside the Jam2 folder. Add relevant native CTests throughout the work; run optimized Release validation before distribution.
8. **First updater-enabled release:** build/upload both OS ZIPs for the chosen version and verify the server's `/version.txt` and both GitHub `.zip.json` assets. The supplied version is authoritative; differing commits do not block publishing. Existing users install this release manually once; later releases use the updater.

Community/documentation work can proceed alongside these steps. Keep the connection experiments separate from the updater implementation.

## Version tracking and updates

- Generate the packaged and compiled versions from the release script's `--version` override, or CMake's development default `0.1.0` when omitted. The override does not edit source files or persist to later ordinary builds. Jam2 checks `https://jam2gether.com/version.txt` asynchronously after startup, requires a successful response, trims whitespace and compares major/minor/patch numerically. The endpoint remains a plain version number; unavailable metadata does not block startup or open an unsolicited error popup. Use **Updates** to check manually or inspect errors.
- Only when the advertised version is newer, GET the current OS's small `.zip.json` asset from that exact GitHub tag. For version `0.2.0`, use `https://github.com/PhilKeeble/Jam2/releases/download/v0.2.0/Jam2-windows.zip.json` or `Jam2-macos.zip.json`. Use the version-specific URL so a newer GitHub Latest release cannot change the selected update mid-check.
- Validate the metadata's schema, version, tag, OS asset name, channel, download URL, byte count and SHA-256 before prompting. Fetching this JSON does not fetch the ZIP; download the ZIP only after user approval.
- Keep exactly one `Jam2-windows.zip` and one `Jam2-macos.zip`, each with its adjacent `.zip.json`. Retain the server's existing version-sync job and website download links; preserve the previous version on failed or incomplete release checks. No additional website endpoint is required.
- The helper downloads the approved package and verifies size, hash, platform and architecture. If the GitHub metadata changes after approval, stop before fetching the ZIP and require a fresh check. Downloading/extraction run in a helper process; staging its runtime uses a worker thread. `package-manifest.json` records package-owned files/hashes (or link targets); its own filename is reserved and not self-hashed. `models.json` pins the model release and weight hashes for that installed app, even when the app is older than the server's release.
- Use one `update/` folder inside Jam2 for the downloaded ZIP, metadata and unpacked `package/`. The running helper and its required Qt runtime/TLS files live in `temp/updater/`. Clear stale download contents before each new download, never while installation is running; only a fully verified package is ready to install. Remove package/ZIP after success and remove the remaining staging/helper files on the next launch. A lock prevents another updater from using the same installation concurrently.
- Offer **Restart and install** or **Install on exit**, with text explaining that restarting closes Jam2, stops the jam and reopens the updated app. A ready update defaults to normal-exit installation without reopening; **Discard update** removes it. Finish saves and stop workers before installation. If the save/close prompt is cancelled, stay open with the update queued. Preparation exits once verified, so deferred updates have no process-wait timeout. Use direct replacement without backups or rollback; report failures, with manual reinstallation as the recovery path.
- Ship a private C++ updater in `components/updater` on Windows and inside the Mac bundle. Jam2 copies it and its Qt dependencies into `temp/updater/` before launching, so it can replace the installed helper and Qt files too. Qt's native TLS backends and the statically compiled miniz reader perform all HTTP and extraction work in process; no curl, tar, PowerShell or other external command is used at runtime. Preserve user data and models, and record the version only after successful replacement.
- Replace owned files on Windows and the whole Mac bundle. Build Windows for Intel/AMD x64 only; Windows Arm support is out of scope. Target a universal Intel/Apple Silicon Mac app, including Qt, workers, updater and ONNX Runtime. Verify HTTPS dependencies and Mac packaging/signing; advertise only validated architectures.

## Manual updater testing

Close other Jam2 instances, or pass `--wait-pid PID` for the GUI instance that
will close normally after saving. The helper waits up to two minutes; it never
kills Jam2 or its workers. Windows locked-file checks run before replacement;
Mac packages retain executable permissions/framework links and undergo native
code-signature verification before the bundle is replaced.

For a published updater-enabled release (substitute its actual version):

```powershell
.\release\jam2.exe debug update --version 0.1.2 --restart
```

On macOS use `./release/Jam2.app/Contents/MacOS/jam2 debug update` with the same
arguments. This explicit command downloads and installs; the GUI now provides
startup checks, approval/progress and restart/normal-exit choices as well.

To test a local package, build with `build-release` and extract the matching OS
ZIP into `release/update/package/`, with `package-manifest.json` directly inside
that folder. Then run:

```powershell
.\release\jam2.exe debug update --version 0.1.2 --check-staged
.\release\jam2.exe debug update --version 0.1.2 --install-staged --restart
```

Both staged modes avoid network requests; `--check-staged` only verifies files.
The version must match the package. Progress is written to `update/status.json`,
details to `temp/updater/updater.log`, and the final result to `update/result.json`.
The next GUI launch reports an unreported failure and cleans temporary runtime
files; failure details/downloads remain under `update/` until the next attempt.
Successful updates clean both dedicated staging folders. Existing releases
without the bundled helper cannot serve as updater-enabled target packages.
For a live GUI test, publish a fresh updater-enabled version for both platforms,
refresh the website's `/version.txt`, and run an older updater-enabled build.
Merely having the old pre-updater ZIPs published is insufficient for this test.

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
- `--include-models` additionally creates `dist/Jam2-models.zip` containing only `models/`, with the weights, sidecars and `MODEL_NOTICES.md` inside. Extract it into `components/jamtaster/` inside Jam2 on both platforms. `models.json` stays in the application package and as a separate model release asset; its `archive_root` identifies `models/`, with file paths relative to that directory. An existing ZIP is reused when its model tag/manifest match and its size/hash verify, even when the app version changes. The layout change causes one rebuild/reupload of older model packages. Without the flag, an existing model ZIP is left untouched. Package creation verifies each weight's size/hash against its source sidecar and rejects LFS pointers.
- Each ZIP has an adjacent `.zip.json` recording its channel, version, URL, byte count, SHA-256, upload tag and required model tag/manifest hash. Generated JSON uses the same line endings on both platforms so manifest hashes agree. App tags are `v<version>`; model tags are `models-<content hash>` derived from the required weight list, independent of app version and shared across platforms. Reuse that model release until weights change.
- Mac builds require universal dependencies, including a universal Qt installation selected with `QT_DIR`, and both vendored ONNX dylibs. The script combines ONNX architectures, bundles dependencies, verifies architectures/linkage and uses `ditto` to preserve bundle links/permissions. No certificate is required for local ad-hoc-signed packages. Later use `--sign-identity "Developer ID Application: ..."`; notarization remains a separate step.
- Without `--publish`, scripts only create local artifacts. After publication, the server's existing job refreshes `/version.txt`; Jam2 fetches the matching `.zip.json` directly from GitHub when needed. Only the application/model requirement manifests belong inside the app ZIP; finished archive hashes remain outside it.

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
- GitHub releases must be mutable for clobber. Replacing an asset deletes its previous upload first; a failed upload can leave it absent until retried. Reusing the same version does not notify already-installed clients; the uploaded `.zip.json` must match the replacement ZIP.
- The server advertises the published stable application version. Model downloads follow the installed app's pinned model tag independently of GitHub's Latest badge. Bleeding channels and their version checks remain future work.

## Everything stays in the Jam2 folder

- The extracted folder is the portable root. On macOS, keep mutable data beside `Jam2.app`, outside its signed bundle.
- Store preferences as the single `settings/preferences.ini` file, with `settings/` beside `tracks/` and the other data folders.
- Store VST3 scan results under `temp/` in this root and remove them after scanning.
- Keep Jam2-managed defaults for preferences, models, recordings, songs, tracks, logs, caches, downloads, staging and helper copies inside this root. Report unwritable locations instead of falling back to profile or system temporary folders. Qt's own file-dialog state, user-selected recording/log/export destinations, external JamJar workspaces, and explicit CLI/debug outputs are accepted exceptions.
- Derive paths from the installation, independently of the open project or working directory; prevent extracted archive paths and filesystem links escaping the intended destination.
- Models live at `components/jamtaster/models/` under the portable root on both platforms, outside the signed Mac bundle and separate from `update/`. Use uploaded model release assets rather than LFS downloads or source archives. Mark model releases as not latest (`make_latest: false`) so application discovery and existing latest-download links remain correct.
- Each application pins the model tag, URL, unpacked size and file hashes in `models.json`, generated during ordinary compiles and packaging. The model downloader GETs `Jam2-models.zip.json` from that exact GitHub tag for the compressed size/digest, validates the manifest pin, and waits for approval before fetching the ZIP. Windows and Mac share a model release. Native workers/ONNX Runtime remain in app packages. Existing models are hash-checked; a failed or cancelled setup can be retried without touching user content.
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
