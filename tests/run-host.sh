#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
# The two COP0 reads belong to unused sign-in/playback functions. Keep the
# production loader intact while allowing the translation unit to assemble.
sed 's/__asm__ volatile("mfc0 %0, \$9" : "=r"(count));/count = 0;/' \
    src/jf.c > "$build_dir/jf.c"
${CC:-cc} -std=gnu99 -g -O1 -Wall -Wextra \
    -ffunction-sections -fdata-sections \
    -Itests/stubs -Isrc -I"${JSMN_INCLUDE:-/opt/thirdparty}" \
    tests/episodes.c "$build_dir/jf.c" src/json.c src/text.c \
    -Wl,--gc-sections -o "$build_dir/episodes"
"$build_dir/episodes"
${CC:-cc} -std=gnu99 -g -O1 -Wall -Wextra \
    -Itests/stubs -Isrc tests/tracks.c src/scr_tracks.c -o "$build_dir/tracks"
"$build_dir/tracks"
${CC:-cc} -std=gnu99 -g -O1 -Wall -Wextra \
    -ffunction-sections -fdata-sections -Itests/stubs -Isrc \
    tests/app_input.c src/app.c -Wl,--gc-sections -o "$build_dir/app-input"
"$build_dir/app-input"
python3 tests/font_atlas.py
