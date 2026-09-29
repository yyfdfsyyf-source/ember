#!/usr/bin/env bash
# Cross-build Ember for x86-64 Linux with Zig, so the binary can be produced on
# a machine with no Linux runtime present (this repo's dev box is Windows).
#
# MODE=dynamic (default, works):  target x86_64-linux.<FLOOR>-gnu, linking
#   libcurl.so.4 from the unpacked Alpine sonames. Measured on the produced
#   binary: it needs libcurl.so.4 + libc.so.6 with symbol versions up to
#   GLIBC_2.29 — zig's bundled libc++ reaches past FLOOR, so FLOOR=2.17 does
#   NOT by itself guarantee a 2.17 box can run it.
# MODE=static (blocked):  x86_64-linux-musl -static would give a zero-dependency
#   ELF, but Alpine builds curl-static against c-ares and ships no
#   c-ares-static, and its libpsl.a lacks psl_free/psl_latest, so the .a set
#   cannot satisfy libcurl's own references. Needs curl built from source, or a
#   POSIX http.cpp that shells out to the system curl instead of libcurl.
#
#   usage:  ./build-linux.sh                      # ember
#           BUILD_TESTS=1 ./build-linux.sh        # + the 20 offline test binaries
#           STAGE=1 ./build-linux.sh              # + releases/ember-<version>-linux-x86_64
#           ZIG=... SYSROOT=... ./build-linux.sh  # override tool locations
set -euo pipefail
cd "$(dirname "$0")"

# VERSION at the repo root is the single source of truth for the build version;
# it is passed as a raw, unquoted token and stringized in agent/version.hpp.
VER="$(tr -d '[:space:]' < VERSION)"

ZIG="${ZIG:-/d/tools/zig/zig.exe}"
SYSROOT="${SYSROOT:-/d/tools/linux-sysroot}"
MODE="${MODE:-dynamic}"
FLOOR="${FLOOR:-2.17}"   # zig's glibc floor; see the measured 2.29 caveat above
if [ "$MODE" = dynamic ]; then
  TARGET="${TARGET:-x86_64-linux.${FLOOR}-gnu}"
else
  TARGET="${TARGET:-x86_64-linux-musl}"
fi
OPT="${OPT:--O2}"
OBJ="out-linux/obj-$MODE"
OUT="${OUT:-out-linux/$MODE}"
mkdir -p "$OBJ" "$OUT"

# uia.cpp (UI Automation) and browser.cpp (Winsock CDP client) are Windows-only
# by design; tools.cpp does not register desktop_*/browser_* elsewhere.
COMMON=(tui/src/platform.cpp tui/src/screen.cpp tui/src/renderer.cpp tui/src/input.cpp
        tui/src/widgets.cpp tui/src/md.cpp tui/src/form.cpp tui/src/drawing.cpp
        agent/src/api.cpp agent/src/http.cpp agent/src/tools.cpp agent/src/background.cpp
        agent/src/context.cpp agent/src/plugins.cpp agent/src/patch.cpp agent/src/search.cpp
        agent/src/rag.cpp agent/src/session.cpp agent/src/settings.cpp agent/src/trajectory.cpp
        agent/src/usage.cpp agent/src/cache.cpp)

INC=(-Iagent/include -Itui/include -Ithird_party -I"$SYSROOT/usr/include")
DEFS=(-DEMBER_VERSION="$VER")

build() { # $@ = sources to compile, then link into $OUT/$1name
  local name="$1"; shift
  local objs=()
  for f in "$@"; do
    local o="$OBJ/$(echo "$f" | tr '/.' '__').o"
    # Rebuild on a source or VERSION change: a version bump has to reach the
    # binary, or it keeps reporting the previous number.
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ VERSION -nt "$o" ]; then
      echo "  CXX $f"
      "$ZIG" c++ -target "$TARGET" -std=c++20 "$OPT" "${INC[@]}" "${DEFS[@]}" -c "$f" -o "$o"
    fi
    objs+=("$o")
  done
  echo "  LINK $OUT/$name"
  if [ "$MODE" = dynamic ]; then
    # Link against the Alpine sonames we unpacked; the target machine supplies
    # its own libcurl.so.4 at run time. --allow-shlib-undefined because the
    # unpacked libcurl.so.4 references sonames we did not unpack.
    "$ZIG" c++ -target "$TARGET" -s "${objs[@]}" -o "$OUT/$name" \
      -L"$SYSROOT/usr/lib" -lcurl -Wl,--allow-shlib-undefined \
      -lpthread -ldl -lm
  else
    "$ZIG" c++ -target "$TARGET" -static -s "${objs[@]}" -o "$OUT/$name" \
      -L"$SYSROOT/usr/lib" -Wl,--start-group \
      -lcurl -lssl -lcrypto -lngtcp2 -lnghttp2 -lssh2 -lzstd -lbrotlidec -lbrotlicommon \
      -lidn2 -lpsl -lunistring -lz -Wl,--end-group -lpthread -ldl -lm -lcrypt
  fi
}

echo "== ember $VER =="
build ember "${COMMON[@]}" agent/src/main.cpp agent/src/app.cpp

# Optional release staging, so the artifact is named after VERSION instead of a
# date and both platforms share one version line. The package carries the runtime
# checker and the Linux README from tools/package/templates, so a release does not
# depend on scavenging files out of the previous release directory.
if [ "${STAGE:-0}" = 1 ]; then
  stage="releases/ember-$VER-linux-x86_64"
  mkdir -p "$stage"
  cp "$OUT/ember" "$stage/ember"
  tpl=tools/package/templates
  cp "$tpl/verify.sh" "$stage/verify.sh"
  sed "s/@VERSION@/$VER/g" "$tpl/README-linux.txt" > "$stage/README-linux.txt"
  echo "staged: $stage/ember, verify.sh, README-linux.txt"
fi

if [ "${BUILD_TESTS:-0}" = 1 ]; then
  # Runtime verification has to happen on the target box: these are the offline
  # test binaries, cross-built the same way. test_browser / test_desktop /
  # test_alltools drive CDP and UIA and are not built for Linux at all.
  for t in tests/*.cpp; do
    n=$(basename "$t" .cpp)
    case "$n" in test_browser|test_desktop|test_alltools) continue;; esac
    echo "== $n =="
    extra=()
    if [ "$n" = test_inputflow ]; then extra=(agent/src/app.cpp); fi
    build "$n" "${COMMON[@]}" ${extra[@]+"${extra[@]}"} "$t"
  done
fi

if [ "${STAGE:-0}" = 1 ] && [ "${BUILD_TESTS:-0}" = 1 ]; then
  # The staging block above runs before these are built, so copy them in now.
  for f in "$OUT"/test_*; do
    [ -f "$f" ] && cp "$f" "$stage/"
  done
  echo "staged $(ls "$stage" | wc -l) files into $stage"
fi
