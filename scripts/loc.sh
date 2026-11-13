#!/usr/bin/env bash
# Code-line budget check (code only: no blanks, no comments). Usage: scripts/loc.sh
set -euo pipefail
cd "$(dirname "$0")/.."

count() {  # $1 = path; prints code lines
  if command -v tokei >/dev/null; then
    tokei "$1" -o json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(sum(v["code"] for k,v in d.items() if k!="Total" and k in ("C++","CppHeader","C","Python","Shell")))'
  elif command -v cloc >/dev/null; then
    cloc --quiet --csv "$1" | awk -F, 'END{print $5+0}'
  else  # fallback: strip blanks, //-lines and # comment lines (approximate)
    find "$1" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.py' -o -name '*.sh' \) -print0 |
      xargs -0 -r cat | { grep -Ev '^[[:space:]]*(//|#[^i]|$)' || true; } | wc -l
  fi
}

fail=0
check() {  # name path cap
  local n; n=$(count "$2")
  printf '%-8s %5d / %d\n' "$1" "$n" "$3"
  [ "$n" -le "$3" ] || fail=1
}
for m in util:350 net:600 http:350 store:500 shard:350 persist:550; do
  [ -d "src/${m%%:*}" ] && check "${m%%:*}" "src/${m%%:*}" "${m##*:}"
done
check src src 3500
check tests tests 1200
check bench bench 300
command -v tokei >/dev/null || command -v cloc >/dev/null || echo "(approximate: install tokei for the official count)"
exit $fail
