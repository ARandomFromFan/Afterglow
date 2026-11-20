#!/usr/bin/env bash
# The packaged entry point (flake.nix): lift and build the game from your own dump into WORK_DIR, then run it.
set -eu
share=@share@
usage="usage: afterglow lift|build|run [args]  (set WORK_DIR and GAME_DIR, the dir with exefs/ and romfs/)"
: "${WORK_DIR:?$usage}" "${GAME_DIR:?$usage}"
export WORK_DIR GAME_DIR
cmd=${1:-run}
[ $# -gt 0 ] && shift
case $cmd in
  lift) exec python3 "$share/scripts/lift.py" "$@" ;;
  build)
    python3 "$share/scripts/build.py" "$@"
    # copied, not linked: the game loads the renderer from beside itself, and a store link could be collected
    install -m 755 @lib@/afterglow_vk.so "$WORK_DIR/afterglow_vk.so" ;;
  run) exec "$WORK_DIR/duskbloods_game" "$WORK_DIR/sa/image/image.bin" --romfs "$GAME_DIR/romfs" --user "$WORK_DIR/user" "$@" ;;
  *) echo "$usage" >&2; exit 2 ;;
esac
