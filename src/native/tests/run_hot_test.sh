#!/bin/bash
# Builds and runs the native-replacement difftest (hot_audio_test.cpp) on the host. Small: one generated file.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
DLL=${PROTORE_ENGINE:-$HOME/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll}
OUT=$ROOT/build/hot_test
mkdir -p "$OUT"
printf '105e8860\n105e85b0\n' > "$OUT/functions.txt"
python3 -I "$ROOT/tools/recomp/recomp.py" --functions "$OUT/functions.txt" --out "$OUT/ref.c" --base 0x58000000 \
    --no-native >/dev/null
CONSTS=$(python3 -I - "$DLL" <<'EOF'
import struct, sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
print(" ".join(repr(struct.unpack("<f", pe.get_data(a - 0x10000000, 4))[0]) for a in (0x10e2e9dc, 0x10e2e864, 0x10e313a4)))
EOF
)
FLAGS="-O1 -DRECOMP_FLAT_MEMORY -fsigned-char -fno-strict-aliasing -fwrapv -I$ROOT/src/recomp -I$ROOT/src/native"
gcc -std=c11 $FLAGS -w -include math.h -c "$OUT/ref.c" -o "$OUT/ref.o"
g++ -std=c++17 $FLAGS -c "$ROOT/src/native/hot_audio.cpp" -o "$OUT/hot_audio.o"
g++ -std=c++17 $FLAGS "$ROOT/src/native/tests/hot_audio_test.cpp" "$OUT/ref.o" "$OUT/hot_audio.o" -o "$OUT/hot_audio_test"
echo "constants: $CONSTS"
"$OUT/hot_audio_test" $CONSTS
