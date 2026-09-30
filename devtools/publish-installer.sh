#!/bin/bash
#
# Publishes the installer page (web/installer) on GitHub Pages: builds it
# (make-firmware.sh: both boards' firmware, the page's demos, the test media)
# and pushes the page's files to the gh-pages branch of origin, as one commit
# that replaces the last (the branch holds the build, not a history). GitHub
# serves it at https://anight.github.io/piegpu/ (Settings > Pages: deploy
# from the branch gh-pages, its root).
#
# Needs a clean checkout (the firmware says which commit it's built from),
# Emscripten (the demos: EMSDK, default ~/emsdk) and the test media
# (make-firmware.sh downloads them).
#
#   devtools/publish-installer.sh
#
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
PAGE=$ROOT/web/installer

if ! git -C "$ROOT" diff --quiet HEAD --; then
	echo "publish-installer.sh: the checkout has changes: commit them first" >&2
	exit 1
fi

"$PAGE/make-firmware.sh"

for f in firmware/manifest.json firmware/LICENCE.broadcom demos/gears.js demos/media.js \
	 media/bbb_trailer-480p.mov media/monkeys_spinning_monkeys.mp3; do
	if [ ! -f "$PAGE/$f" ]; then
		echo "publish-installer.sh: no $f: the page would be incomplete" >&2
		exit 1
	fi
done

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
cp "$PAGE"/index.html "$PAGE"/*.js "$OUT/"
cp -r "$PAGE/firmware" "$PAGE/demos" "$PAGE/media" "$OUT/"
cp "$ROOT/LICENSE" "$OUT/"
touch "$OUT/.nojekyll"			# (served as they are)

VERSION=$(python3 -c "import json, sys; m = json.load (open (sys.argv[1])); print (m['version'], m.get ('git', ''))" \
	  "$PAGE/firmware/manifest.json")
git -C "$OUT" init -q -b gh-pages
git -C "$OUT" add -A
git -C "$OUT" -c user.name="$(git -C "$ROOT" config user.name)" -c user.email="$(git -C "$ROOT" config user.email)" \
	commit -q -m "The installer page: piegpu $VERSION"
git -C "$OUT" push -q --force "$(git -C "$ROOT" remote get-url origin)" gh-pages
echo "publish-installer.sh: pushed gh-pages: piegpu $VERSION, $(du -sh --exclude=.git "$OUT" | cut -f1)"
