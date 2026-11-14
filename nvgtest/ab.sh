#!/bin/bash
# A/B test of nanovgXC changes: renders all nvgtest scenes w/ nanovgXC/src from BASE_REV (default HEAD) and w/
#  the working tree, then compares exactly.  Only nanovgXC/src is taken from BASE_REV; nvgtest itself, glad, stb
#  and fonts come from the working tree.
# usage: ab.sh [-r BASE_REV | -s BASE_SRC_DIR] [-b NFRAMES] [-a] [-o OUTDIR] [-- extra nvgtest args]
#  -s BASE_SRC_DIR: use this directory (containing nanovg.c etc.) as baseline instead of a git revision, e.g. to
#   isolate one of several uncommitted changes
#  -b NFRAMES: also benchmark both builds after comparing, alternating base and new ROUNDS times (default 2) per
#   config, w/ args from BENCH_ARGS (';'-separated groups; default: text pages at 4K w/ draws submitted 10x per
#   frame, SVG and main path scenes at 1080p 5x), configs
#   from BENCH_CONFIGS (default all); prints a summary of median "flush" time over rounds
#  -a: run on Android device (adb; arm64): builds w/ NDK, pushes to /data/local/tmp/nvgtest, pulls images back
# exit code 1 if any image differs
set -e
set -f  # no globbing: scene patterns like write_* are passed to nvgtest
HERE=$(cd "$(dirname "$0")" && pwd)
NVGXC=$(cd "$HERE/../../nanovgXC" && pwd)
BASE=HEAD
BASESRC=
BENCH=0
ANDROID=0
OUT=$HERE/out
# ';'-separated groups of nvgtest args, each run separately: text is cheap per pixel so needs a large frame and many
#  repeats; paths are much more expensive
BENCH_ARGS=${BENCH_ARGS:---size 3840x2160 --gpu-repeat 10 --scene text_page12 --scene text_page24;\
--size 1920x1080 --gpu-repeat 5 --scene svg_tiger --scene write_* --scene paths_big --scene paths_small --scene paths_axis}
BENCH_CONFIGS=${BENCH_CONFIGS:-sdf-srgb sdf-linear summed-srgb}
ROUNDS=${ROUNDS:-2}
DEV=/data/local/tmp/nvgtest
while getopts "r:s:b:o:a" opt; do
  case $opt in
    r) BASE=$OPTARG ;;
    s) BASESRC=$(cd "$OPTARG" && pwd) ;;
    b) BENCH=$OPTARG ;;
    o) OUT=$OPTARG ;;
    a) ANDROID=1 ;;
    *) sed -n '2,14p' "$0"; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ "$1" = "--" ] && shift

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
# baseline src/: extracted from git (no worktree needed) or copied from BASESRC; built in the same place either
#  way so object files and paths don't depend on which
rm -rf "$OUT/base-src"
mkdir -p "$OUT/base-src"
if [ -n "$BASESRC" ]; then
  cp -r "$BASESRC" "$OUT/base-src/src"
  echo "base: $BASESRC; new: working tree"
else
  git -C "$NVGXC" archive "$BASE" src | tar -x -C "$OUT/base-src"
  echo "base: nanovgXC $(git -C "$NVGXC" rev-parse --short "$BASE"); new: working tree"
fi
git -C "$NVGXC" status --short src

# host build is always needed for --compare
make -s -C "$HERE" -j2 BUILDDIR="$OUT/build-host"
if [ $ANDROID -ne 0 ]; then
  adb get-state >/dev/null || { echo "no Android device"; exit 2; }
  echo "device: $(adb shell getprop ro.product.model) ($(adb shell getprop ro.hardware))"
  make -s -C "$HERE" -j2 ANDROID=1 NVGSRC="$OUT/base-src/src" BUILDDIR="$OUT/build-base-android"
  make -s -C "$HERE" -j2 ANDROID=1 BUILDDIR="$OUT/build-new-android"
  adb shell "rm -rf $DEV && mkdir -p $DEV"
  adb push "$OUT/build-base-android/nvgtest" "$DEV/nvgtest-base" >/dev/null
  adb push "$OUT/build-new-android/nvgtest" "$DEV/nvgtest-new" >/dev/null
  for f in "$NVGXC/example/fonts/Roboto-Regular.ttf" "$NVGXC/example/svg/tiger.svg" "$NVGXC/example/Opt_page1.svg" \
      "$NVGXC/../scribbleres/Intro.svg"; do
    adb push "$f" "$DEV/" >/dev/null
  done
else
  make -s -C "$HERE" -j2 NVGSRC="$OUT/base-src/src" BUILDDIR="$OUT/build-base"
  make -s -C "$HERE" -j2 BUILDDIR="$OUT/build-new"
fi

# run BUILD (base or new) w/ remaining args; on Android, --out must be a device path
run() {
  local build=$1; shift
  if [ $ANDROID -ne 0 ]; then
    local args="" a
    for a in "$@"; do args="$args '$a'"; done  # quote for device shell (scene patterns)
    adb shell "cd $DEV && ./nvgtest-$build $args" 2>/dev/null
  else
    "$OUT/build-$build/nvgtest" "$@" 2>/dev/null
  fi
}

# configs: name and nvgtest args
CONFIGS=("sdf-srgb:--sdf 1 --srgb 1" "sdf-linear:--sdf 1 --srgb 0" "summed-srgb:--sdf 0 --srgb 1")
STATUS=0
for cfg in "${CONFIGS[@]}"; do
  name=${cfg%%:*}
  args=${cfg#*:}
  rm -rf "$OUT/base/$name" "$OUT/new/$name" "$OUT/diff/$name"
  mkdir -p "$OUT/base" "$OUT/new" "$OUT/diff"
  for build in base new; do
    if [ $ANDROID -ne 0 ]; then
      run $build $args --out "$DEV/out-$build-$name" "$@" >/dev/null
      adb pull "$DEV/out-$build-$name" "$OUT/$build/$name" >/dev/null
    else
      run $build $args --out "$OUT/$build/$name" "$@"
    fi
  done
  echo "== $name"
  "$OUT/build-host/nvgtest" --compare "$OUT/base/$name" "$OUT/new/$name" --diff "$OUT/diff/$name" || STATUS=1
done

if [ "$BENCH" -gt 0 ]; then
  BENCHLOG="$OUT/bench.txt"
  : > "$BENCHLOG"
  for cfg in "${CONFIGS[@]}"; do
    name=${cfg%%:*}
    args=${cfg#*:}
    [[ " $BENCH_CONFIGS " == *" $name "* ]] || continue
    echo "== bench $name"
    for ((round = 0; round < ROUNDS; ++round)); do
      for build in base new; do
        echo "-- $build"
        IFS=';' read -ra BGROUPS <<< "$BENCH_ARGS"
        for bargs in "${BGROUPS[@]}"; do
          run $build $args --bench "$BENCH" $bargs | tee /dev/stderr | sed "s/^/$name $build /" >> "$BENCHLOG"
        done
      done
    done 2>&1
  done
  # summary: median over rounds of each run's median flush time
  echo "== summary: median flush (ms) over $ROUNDS rounds"
  awk '{ key = $1 " " $3; v[key, $2, ++n[key, $2]] = $6; keys[key] = 1 }
    function med(k, b,   m, i, j, t, a) {
      m = n[k, b]; for (i = 1; i <= m; ++i) a[i] = v[k, b, i]
      for (i = 1; i <= m; ++i) for (j = i + 1; j <= m; ++j) if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
      return m % 2 ? a[(m + 1)/2] : (a[m/2] + a[m/2 + 1])/2 }
    END { for (k in keys) { b = med(k, "base"); nw = med(k, "new")
      printf "%-28s base %8.3f  new %8.3f  new/base %.3f\n", k, b, nw, nw/b } }' "$BENCHLOG" | sort
fi

[ $STATUS -eq 0 ] && echo "all images identical" || echo "images differ - see $OUT/diff"
exit $STATUS
