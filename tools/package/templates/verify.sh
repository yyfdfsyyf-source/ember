#!/bin/sh
# Run this ON THE LINUX BOX. It says in one screen whether the build works there.
# Nothing here needs a network or an API key except the two lines marked NET.
cd "$(dirname "$0")" || exit 1

echo "== 环境 =="
if command -v ldd >/dev/null 2>&1; then ldd --version 2>/dev/null | head -1; fi
uname -srm
grep -c avx2 /proc/cpuinfo 2>/dev/null | sed 's/^/avx2 支持(0=没有，不影响本二进制): /'
echo "libcurl.so.4:"; (ldconfig -p 2>/dev/null | grep -m1 curl) || ls /usr/lib/*/libcurl.so.4 /usr/lib/libcurl.so.4 2>/dev/null || echo "  未找到 —— 需要装 libcurl4 包"

echo
echo "== 冒烟 =="
./ember --help 2>&1 | head -3

echo
echo "== 离线测试 =="
fails=0
for t in test_*; do
  [ -x "./$t" ] || continue
  out=$(./"$t" 2>&1 | tail -1)
  case "$out" in
    *"failures=0"*|*"0 failures"*|*"all"*"passed"*|*PASSED*|*OK*) printf "  ok    %-20s %s\n" "$t" "$out" ;;
    *) printf "  FAIL  %-20s %s\n" "$t" "$out"; fails=$((fails+1)) ;;
  esac
done
echo "失败测试数: $fails"
