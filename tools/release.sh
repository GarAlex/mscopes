#!/usr/bin/env bash
#
# Build a Developer ID-signed, notarized MScopes.dmg for direct distribution.
#
#   tools/release.sh                  # full: build, sign, notarize, staple, dmg
#   NOTARY_PROFILE=name tools/release.sh
#
# Prerequisites (one-time, yours to do):
#   - a "Developer ID Application" certificate in the login keychain
#   - notarytool credentials stored as a keychain profile:
#       xcrun notarytool store-credentials mscopes-notary \
#           --apple-id "<Apple ID>" --team-id T9Y6669YYQ
#     (prompts for an app-specific password from appleid.apple.com)
# Without the profile the script still builds, signs and packages, and tells
# you the exact notarize/staple commands to run afterwards.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT="$REPO/build/release"
SIGN_ID="${SIGN_ID:-Developer ID Application: Alexander Garmash (T9Y6669YYQ)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-mscopes-notary}"

rm -rf "$OUT"; mkdir -p "$OUT"

echo "== 1/5  Release build (universal) =="
( cd "$REPO/apple" && xcodegen generate > /dev/null )
xcodebuild -project "$REPO/apple/MScopes.xcodeproj" -scheme App -configuration Release \
    -destination 'generic/platform=macOS' -derivedDataPath "$OUT/DerivedData" build \
    > "$OUT/xcodebuild.log" 2>&1 || { tail -30 "$OUT/xcodebuild.log"; exit 1; }
APP="$OUT/MScopes.app"
cp -R "$OUT/DerivedData/Build/Products/Release/MScopes.app" "$APP"
VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")
echo "   MScopes $VERSION: $(lipo -info "$APP/Contents/MacOS/MScopes" | sed 's/.*are: //')"

echo "== 2/5  Sign with Developer ID (plugin first, then the app) =="
codesign --force --options runtime --timestamp --sign "$SIGN_ID" \
    "$APP/Contents/PlugIns/MScopesPlugin.bundle"
codesign --force --options runtime --timestamp \
    --entitlements "$REPO/apple/App/MScopes.entitlements" --sign "$SIGN_ID" "$APP"
codesign --verify --deep --strict "$APP"
echo "   signed: $(codesign -dvv "$APP" 2>&1 | grep "^Authority" | head -1 | sed "s/Authority=//")"

echo "== 3/5  Notarize =="
ZIP="$OUT/MScopes-$VERSION.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
notarized=0
if xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" > /dev/null 2>&1; then
    xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$APP"
    notarized=1
else
    echo "   (no notarytool profile '$NOTARY_PROFILE' — skipping; see the header of this script)"
fi

echo "== 4/5  Disk image =="
STAGE="$OUT/dmg"; mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
DMG="$OUT/MScopes-$VERSION.dmg"
hdiutil create -volname "MScopes" -srcfolder "$STAGE" -ov -format UDZO "$DMG" > /dev/null
codesign --force --timestamp --sign "$SIGN_ID" "$DMG"
if [[ $notarized -eq 1 ]]; then
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
fi

echo "== 5/5  Gatekeeper check =="
spctl --assess --type execute --verbose=2 "$APP" 2>&1 | sed 's/^/   /' || true
echo
echo "   $DMG"
if [[ $notarized -eq 0 ]]; then
    cat <<MSG
   Not notarized yet. After storing credentials, run:
     xcrun notarytool submit "$ZIP" --keychain-profile $NOTARY_PROFILE --wait
     xcrun stapler staple "$APP"
     xcrun notarytool submit "$DMG" --keychain-profile $NOTARY_PROFILE --wait
     xcrun stapler staple "$DMG"
MSG
fi
