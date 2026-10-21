#!/bin/bash
# Packages the APK without Gradle: SDL's Java activity + ours -> classes.dex, the manifest -> aapt2, native
# libraries for the requested ABIs, then zipalign and apksigner (debug key).
#
# Usage: android/build-apk.sh [abi ...]          (default: arm64-v8a)
# Needs: build/android/<abi>/libmain.so (make -C src/native PLATFORM=android ABI=<abi>) and the libraries from
# android/build-deps.sh in $DEPS_ROOT/<abi>/lib. Output: build/apk/prototype.apk
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
BUILD_TOOLS=${BUILD_TOOLS:-$SDK/build-tools/36.0.0}
PLATFORM_JAR=${PLATFORM_JAR:-$SDK/platforms/android-35/android.jar}
DEPS_ROOT=${DEPS_ROOT:-$HOME/proto-re-data/android-deps}
SDL_SRC=${SDL_SRC:-$HOME/proto-re-data/android-deps-src/SDL2}
KEYSTORE=${KEYSTORE:-$HOME/.android/debug.keystore}
ABIS=("${@:-arm64-v8a}")

OUT=$ROOT/build/apk
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/pkg"

# Java: SDL's activity classes plus ProtoActivity.
javac --release 11 -nowarn -cp "$PLATFORM_JAR" -d "$OUT/classes" \
    "$SDL_SRC"/android-project/app/src/main/java/org/libsdl/app/*.java "$ROOT"/android/java/com/protore/game/*.java
"$BUILD_TOOLS/d8" --release --min-api 26 --lib "$PLATFORM_JAR" --output "$OUT/dex" \
    $(find "$OUT/classes" -name '*.class')

"$BUILD_TOOLS/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM_JAR" --manifest "$ROOT/android/AndroidManifest.xml" \
    --min-sdk-version 26 --target-sdk-version 35

cp "$OUT/dex/classes.dex" "$OUT/pkg/"
for abi in "${ABIS[@]}"; do
    mkdir -p "$OUT/pkg/lib/$abi"
    cp "$ROOT/build/android/$abi/libmain.so" "$DEPS_ROOT/$abi"/lib/lib{SDL2,FAudio,dxvk_d3d9}.so "$OUT/pkg/lib/$abi/"
    "$SDK"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-unneeded "$OUT/pkg/lib/$abi"/*.so
done
(cd "$OUT/pkg" && zip -q -r "$OUT/base.apk" classes.dex lib)

"$BUILD_TOOLS/zipalign" -P 16 -f 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/prototype.apk" "$OUT/aligned.apk"
ls -la "$OUT/prototype.apk"
