# Jam2 releases

Jam2 ships one public executable. Windows uses a portable application ZIP;
macOS uses a self-contained application in an Apple Installer PKG.

## Daily macOS development and installed-package testing

Run these commands from the repository root for the usual local workflow:

```sh
bash ./compile.sh
open "$PWD/release/Jam2.app"
```

Set `QT_DIR` if using an SDK other than Homebrew Qt. Compilation stages the app
in `release/` and keeps local test data there. It does not install into
`/Applications` or use the installed app's Application Support profile.

| Workflow | App | Default writable data |
| --- | --- | --- |
| Compile and run from this checkout | `release/Jam2.app` | `release/` |
| Install the release PKG | `/Applications/Jam2.app` | `~/Library/Application Support/Jam2/` |

Source builds create `.jam2-development-data` beside the app to select local
data. The PKG contains only `Jam2.app`, never this marker or the local test
data. Packaging leaves the marker in `release/`, so you can keep using that app
for development even after building a signed package. Both workflows use the
same storage code and bundle resources; only the default writable root differs.
Explicitly selected project, log and capture destinations still take precedence.

To test the production installation, first build the signed PKG below, close
running Jam2 instances, then run:

```sh
open "$PWD/dist/Jam2-macos.pkg"
```

Complete Apple's Installer and wait for its success screen, then launch the
installed copy explicitly:

```sh
open "/Applications/Jam2.app"
"/Applications/Jam2.app/Contents/MacOS/jam2" --version
```

Check starting/saving a jam, recording, model download or manual model ZIP
installation, and reopening the saved JamJar. Default files should appear in
Application Support. The installed app's settings/models are separate from the
checkout's `release/` files. Use a separate macOS user account to test a fresh
production profile without resetting an existing one. Return to development by
quitting the installed copy and opening `$PWD/release/Jam2.app`; compiling again
leaves the installed app alone. Quit one copy before switching so macOS does not
activate an already-running Jam2 instance instead of the requested app.

An in-app update started from the source copy installs the public app into
`/Applications`; it does not replace the source checkout. Use the installed
copy for production update testing. The helper stages the download in
`release/update/`. After Installer finishes, open `/Applications/Jam2.app`;
the old `release/Jam2.app` remains a source build and may offer the update again.
Source-build settings and downloaded models are not automatically copied into
the installed profile. Existing JamJars can still be opened from their current
locations; models can be installed/downloaded separately for either profile.

## macOS build and publish commands on the configured release Mac

The following uses the SDK, signing identities and existing Keychain profile
validated on this Mac. Run it from the repository root. On another build Mac,
adjust the SDK prefix and use that machine's imported signing identities/profile.

```sh
export QT_DIR="$PWD/build/deps/Qt/6.11.1/macos"
export JAM2_SIGN_IDENTITY="Developer ID Application: Philip Keeble (RH9HPSJYY2)"
export JAM2_INSTALLER_SIGN_IDENTITY="Developer ID Installer: Philip Keeble (RH9HPSJYY2)"
export JAM2_NOTARY_PROFILE="jam2-notary"
JAM2_NEXT_VERSION=0.1.1
```

Choose the next release version instead of blindly reusing the example. Use the
same version on Windows. Keep the exports in the shell used for the commands
below; they contain names only, with credentials and private keys in Keychain.
The existing `jam2-notary` profile is shared by app and PKG notarization; it
does not need to be recreated for each certificate or build.

Build a signed, notarized local package and its separate model assets:

```sh
bash ./build-release.sh --version "$JAM2_NEXT_VERSION" --include-models
pkgutil --check-signature dist/Jam2-macos.pkg
xcrun stapler validate dist/Jam2-macos.pkg
spctl --assess --type install --verbose=2 dist/Jam2-macos.pkg
```

Expect a Developer ID Installer signature and Gatekeeper's
`source=Notarized Developer ID`. Test installation using the workflow above.
`--include-models` prepares the required separate model ZIP and metadata;
unchanged model archives are reused. It does not put models in the app or PKG.

Before distribution, run the full optimized Release suite explicitly and
review its results (see [Validation](docs/Validation.md)):

```sh
bash ./compile.sh --tests-full
```

Compilation refreshes `release/Jam2.app` as a development build; it does not
change a previously built PKG or `/Applications/Jam2.app`. Build/test the final
signed package after any source changes. After validation, publish from the
same shell with:

```sh
gh auth login
bash ./build-release.sh --version "$JAM2_NEXT_VERSION" --include-models --publish
```

`gh auth login` is only needed once per build machine. The release script
rebuilds, signs, notarizes and uploads the Mac PKG and metadata. Repeat the PKG
verification commands on the final output. Upload the Windows assets for the
same version with `./build-release.ps1 --version 0.1.1 --include-models --publish`
from PowerShell on Windows. The release stays draft until both platforms and
their pinned model assets pass the checks described below. Complete the website
transition below before advertising the first PKG release.

## macOS installation and data

The PKG installs only `/Applications/Jam2.app`. Its payload is not relocatable,
replaces the previous app bundle as a whole, and requires running Jam2 instances
to close. Executables, Qt/TLS, native workers, ONNX Runtime, licenses and the
required `Contents/Resources/models.json` manifest live inside the signed app.
No installer script creates per-user folders or launches Jam2 as root.

Distributed Jam2 creates `~/Library/Application Support/Jam2/` as the logged-in user when
needed. Settings (`settings/preferences.ini`), logs, `tracks/`, `songs/`,
`captures/`, caches, plugin-scan temporary files, model downloads and updater
staging live there. Explicitly selected project and recording destinations
remain valid. Source builds use the same subfolders under `release/` instead.
Windows retains the equivalent folders beside `jam2.exe`.

On macOS, adjacent or older profile preferences are copied once if the new INI
is absent; existing default log/capture paths are redirected to user storage.
Custom destinations remain unchanged. Existing projects and model weights in an
old extracted release are preserved there. Open old JamJars from that location,
or copy the desired `songs/` and `components/jamtaster/models/` contents into
the corresponding Application Support directories without overwriting newer
work. Arbitrary old extraction folders cannot be discovered after installing a
PKG elsewhere.

The model ZIP remains `Jam2-models.zip`, containing `models/`, weights, sidecars
and notices. The in-app downloader verifies its pinned manifest and installs to
`~/Library/Application Support/Jam2/components/jamtaster/models/`. For manual
extraction, place the ZIP's `models/` folder in
`~/Library/Application Support/Jam2/components/jamtaster/`. It never belongs
inside `Jam2.app` or directly in `/Applications`. Already installed, verified
weights are reused. For source-build testing, extract into
`release/components/jamtaster/` instead.

## Windows ZIPs and local Mac inspection packages

Use the signed Mac commands above for production installation testing and
publication. The generic Mac command here is also useful for unsigned inspection
when signing/notarization options and environment settings are absent.

Run from the repository root:

```powershell
.\build-release.ps1 --version 0.8.1 --include-models
```

```sh
QT_DIR="$HOME/Qt/6.x/macos" bash ./build-release.sh --version 0.8.1 --include-models
```

Use the actual universal Qt SDK prefix. Both scripts use the optimized Release
build. Outputs are `dist/Jam2-windows.zip` or `dist/Jam2-macos.pkg`, with matching
`.zip.json` or `.pkg.json` metadata. `--include-models` additionally creates the
separate, reusable model ZIP and metadata. Model weights and user data are
excluded from app packages. The compiled version is authoritative; changing a
loose `version.txt` does not change update comparisons.

Without signing options or their environment settings, a local Mac package contains an ad-hoc signed app and
an unsigned PKG for inspection/testing. It is not accepted by the public updater.
Public macOS releases require both Developer ID signatures and notarization;
`--publish` rejects missing signing identities or a missing notary profile.

## Set up the macOS Installer certificate

The application certificate and Installer certificate have separate purposes.
An existing Developer ID Application certificate can be reused for Jam2.app.
Create the missing Developer ID Installer certificate on the build Mac:

1. In Keychain Access, choose **Certificate Assistant > Request a Certificate
   from a Certificate Authority**. Enter your email and a name, leave CA Email
   blank, select **Saved to disk**, and save the CSR. Keep the private key that
   Keychain Access creates on this Mac.
2. In [Apple Developer Certificates](https://developer.apple.com/account/resources/certificates/list),
   create a **Developer ID Installer** certificate for the same team as the
   application certificate. Upload the CSR, generate the certificate and
   download the `.cer` file.
3. Double-click the `.cer` file to add it to the login keychain. Under
   **My Certificates**, it must have an expandable private key beneath it.
   A certificate alone, without its matching private key, cannot sign a PKG.
4. Run `security find-identity -v` and record the full Application and Installer
   identity names. Do not filter with `-p codesigning`, which omits Installer
   identities. Use the same Apple developer team for both.

Apple documents [CSR creation](https://developer.apple.com/help/account/certificates/create-a-certificate-signing-request)
and [Developer ID certificates](https://developer.apple.com/help/account/certificates/create-developer-id-certificates/).
If the signing key was created on another Mac, import its certificate and
matching private key from a securely transferred Keychain `.p12` export instead.

## Store notarization credentials in Keychain

Create an app-specific password for the Apple Account used for notarization,
then run this locally, replacing the account and team placeholders:

```sh
xcrun notarytool store-credentials "jam2-notary" --apple-id "YOUR_APPLE_ID" --team-id "YOUR_TEAM_ID"
xcrun notarytool history --keychain-profile "jam2-notary"
```

Enter the app-specific password only at the interactive prompt. The release
script stores just the profile name, not account credentials or passwords.
An existing working profile for the same team can be reused for both the app
and Installer package, including after adding the Installer certificate.
See [Apple's notarization credential instructions](https://developer.apple.com/documentation/technotes/tn3147-migrating-to-the-latest-notarization-tool).

Build a signed, notarized package using the exact identity names from Keychain:

```sh
QT_DIR="$HOME/Qt/6.x/macos" bash ./build-release.sh --version 0.8.1 \
  --sign-identity "Developer ID Application: YOUR_NAME (YOUR_TEAM_ID)" \
  --installer-sign-identity "Developer ID Installer: YOUR_NAME (YOUR_TEAM_ID)" \
  --notarize-profile "jam2-notary"
```

Alternatively set `JAM2_SIGN_IDENTITY`, `JAM2_INSTALLER_SIGN_IDENTITY` and
`JAM2_NOTARY_PROFILE`. The script signs nested code and the app, notarizes and
staples the app, builds/signs the PKG, notarizes/staples it, checks its signature
and Gatekeeper assessment, and finally hashes the finished PKG for metadata.
Failed signing/notarization never produces new publication metadata.
The final Installer screen tells users to reopen Jam2 from Applications.

## Publishing and the website transition

Run the full optimized Release CTest suite before distribution, review failures,
and validate a real signed/notarized installation. Authenticate GitHub CLI with
`gh auth login`. Add `--publish` to the signed build command when ready.

Publishing uploads the current platform package and metadata to `v<version>`.
A new release stays draft until the Windows ZIP, macOS PKG, matching metadata,
and required pinned model releases are all available. Unchanged model releases
are reused. Existing publication/cleanup rules retain Git tags, model releases,
prereleases and unrelated assets. Upload failures do not publish an incomplete
new release. The website's version.txt continues to advertise stable versions.

**Required change in the separate website repository before this transition:**
update the version-sync job in its `deploy.yml` to require `Jam2-macos.pkg`
instead of `Jam2-macos.zip`, and change the Mac download link to
`https://github.com/PhilKeeble/Jam2/releases/latest/download/Jam2-macos.pkg`.
If that repository checks companion metadata, use `Jam2-macos.pkg.json`.
Deploy it and refresh `jam2gether-version-sync.service`; otherwise version.txt
can remain on the old release and clients will not discover the PKG update.
The website repository is not part of this checkout.

Old ZIP-only Mac updaters cannot consume PKG metadata. Existing users install
the first PKG release manually once; builds containing the new updater use
PKGs for subsequent releases. No Mac application ZIP is published by this flow.

## Updating on macOS

Updates checks the website version and fetches the exact GitHub tag's
`Jam2-macos.pkg.json`. The user approves a download while the jam continues.
The helper verifies size/SHA-256, the Developer ID Installer signature and
Gatekeeper acceptance. At **Quit and Open Installer**, normal project-save
prompts run first. Cancelling leaves Jam2 open; **Later** never opens Installer
on an unrelated normal exit. The next launch re-verifies the retained PKG
without downloading it again. After Jam2 exits, the helper re-verifies the PKG
and opens Apple's Installer. Installer handles authorization, progress,
cancellation and its final success/error screen. Jam2 remains closed.

Opening Installer is recorded as a handoff, not a successful update. Staging
remains available until a later launch confirms the expected compiled version;
then it is removed after Installer closes, without touching settings/models/projects.
Another download or cancellation cannot clear an existing PKG while Installer
is open; private helper files can still be cleaned. A launch of an
older copy explains that installation may still need completion and directs the
user to Applications. `jam2 debug update --version X.Y.Z --check-staged` verifies
a prepared package offline; `--install-staged` opens Installer after verification.
On Windows the existing ZIP install-on-exit/restart flow is unchanged.

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
