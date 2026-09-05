# Signing & distribution

MScopes ships **outside the App Store** (the sandbox blocks the system-wide
CoreAudio tap the app is built on, and a Music visual plugin can't be a store
product at all). Direct distribution means: Developer ID signature, hardened
runtime, notarization, a DMG. `tools/release.sh` does all of it.

## Local builds

`apple/project.yml` uses Automatic signing with the team **T9Y6669YYQ** and an
*Apple Development* certificate — Xcode ▸ Run works as-is. First launch asks
once for permission to capture system audio (`NSAudioCaptureUsageDescription`
in `Info.plist`; entitlement `com.apple.security.device.audio-input` in
`MScopes.entitlements`). Build from the shell with the *generic* destination
so every slice is built (the plugin must be universal):

    cd apple && xcodegen generate
    xcodebuild -project MScopes.xcodeproj -scheme App -configuration Debug \
               -destination 'generic/platform=macOS' build

To build on another team, change `DEVELOPMENT_TEAM` in `project.yml` (or pass
`DEVELOPMENT_TEAM=… ` to xcodebuild).

## Release: one-time setup

1. **Developer ID Application certificate.** Xcode ▸ Settings ▸ Accounts ▸
   your team ▸ Manage Certificates ▸ "+" ▸ *Developer ID Application*. Only the
   account holder can create one. Back up the identity (`.p12`). This is the
   only certificate a DMG release needs; *Developer ID Installer* is for
   `.pkg` files, *Mac App/Installer Distribution* are App Store certs.
2. **Notarization credentials.** Create an app-specific password at
   appleid.apple.com (Sign-In and Security ▸ App-Specific Passwords), then
   store it once — interactively, it prompts for the password:

       xcrun notarytool store-credentials mscopes-notary \
           --apple-id "you@example.com" --team-id T9Y6669YYQ

No App ID registration, App Store Connect record or provisioning profile is
involved; the bundle ids (`com.writea.viz`, `com.writea.viz.musicplugin`) are
plain reverse-DNS identifiers and must simply never change (TCC permissions
and user defaults hang off them).

## Release: every time

    tools/release.sh

builds a universal Release app, signs the embedded plugin then the app with
Developer ID + hardened runtime + a secure timestamp, submits a zip to the
notary service and staples the ticket, wraps it in `MScopes-<version>.dmg`
(signed, notarized, stapled), and prints Gatekeeper's verdict. Output lands in
`build/release/`. Without the notary profile it still builds, signs and
packages, and prints the notarize/staple commands to run afterwards.

`SIGN_ID` and `NOTARY_PROFILE` override the identity and profile name.

## Notes

- A notarized app keeps running after its certificate expires: the timestamp
  proves when it was signed. A fresh certificate is needed only to sign new
  releases.
- The Music plugin is signed and notarized as part of the app bundle; when the
  app copies it into `~/Library/iTunes/iTunes Plug-ins`, the signature travels
  with it.
- Gatekeeper's `spctl --assess` reports "rejected" for a Developer ID app
  until it is notarized — expected, not a signing failure.

## Publishing a release

`tools/release.sh` leaves two identical images in `build/release/`:
`MScopes-<version>.dmg` and `MScopes.dmg`. Create the GitHub release
(tag `v<version>`) and attach **both**. The website and the README link the
constant name through GitHub's permanent redirect,
`https://github.com/GarAlex/mscopes/releases/latest/download/MScopes.dmg`,
so nothing outside the repository changes from release to release. Bump
`MARKETING_VERSION` and `CURRENT_PROJECT_VERSION` in `apple/project.yml`
first; the DMG takes its name from them.
