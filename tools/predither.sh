#!/usr/bin/env bash
# Dither photos on your computer for the PhotoPainter (Spectra E6) and write files the
# device shows 1:1: a panel-sized BMP in pure palette colours plus a "<file>.bmp.cfg"
# that tells epaper_gallery not to touch it again (palette waveshare, no dithering).
#
# Requires: epaper-image-convert (npm install -g @aitjcize/epaper-image-convert)
# Optional: ImageMagick (`magick`) for EXIF-correct portrait/landscape detection,
#           otherwise macOS `sips` is used (ignores EXIF rotation).
#
# Usage:
#   tools/predither.sh <photo|folder> [more photos/folders...] -o <outdir> [-- converter options]
#
# Without converter options it uses: -p balanced --color-method lab --dither-algorithm stucki
# Examples:
#   tools/predither.sh ~/Bilder/Hochzeit -o ~/Bilder/epaper
#   tools/predither.sh foto.jpg -o out -- -p dynamic --dither-algorithm floyd-steinberg
#   tools/predither.sh foto.jpg -o out -- --scale-mode cover   # crop to fill instead of borders
#
# Then upload the .bmp AND the .bmp.cfg into the gallery folder (http://photopainter.local/file/...).

set -euo pipefail

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

inputs=()
outdir=""
extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    -o) outdir="${2:-}"; shift 2 ;;
    -h|--help) usage ;;
    --) shift; extra=("$@"); break ;;
    *) inputs+=("$1"); shift ;;
  esac
done
[ ${#inputs[@]} -gt 0 ] && [ -n "$outdir" ] || usage
[ ${#extra[@]} -gt 0 ] || extra=(-p balanced --color-method lab --dither-algorithm stucki)

command -v epaper-image-convert >/dev/null || {
  echo "epaper-image-convert not found: npm install -g @aitjcize/epaper-image-convert" >&2
  exit 1
}

# prints "W H" as the photo is displayed (EXIF orientation applied when ImageMagick is there)
dimensions() {
  if command -v magick >/dev/null; then
    magick "$1[0]" -auto-orient -format '%w %h' info: 2>/dev/null && return
  fi
  if command -v sips >/dev/null; then
    sips -g pixelWidth -g pixelHeight "$1" 2>/dev/null | awk '/pixelWidth/{w=$2} /pixelHeight/{h=$2} END{if (w) print w, h}'
  fi
}

convert_one() {
  local src="$1" name dims w h size out
  name="$(basename "${src%.*}")"
  dims="$(dimensions "$src")"
  read -r w h <<<"${dims:-1 0}"
  # exact panel size, so the device copies pixels 1:1; portrait stays portrait and the
  # device rotates it like any other portrait photo. fit = white borders like the gallery's
  # default (pass `-- --scale-mode cover` to crop instead; later options win)
  if [ "$h" -gt "$w" ]; then size=480x800; else size=800x480; fi
  mkdir -p "$outdir"
  out="$outdir/$name.bmp"
  echo "→ $src ($size)"
  epaper-image-convert "$src" "$out" -f bmp -d "$size" --scale-mode fit "${extra[@]}" >/dev/null
  cat >"$out.cfg" <<'EOF'
palette=waveshare
dither=none
tone_compression=0.00
saturation=1.00
EOF
}

count=0
for item in "${inputs[@]}"; do
  if [ -d "$item" ]; then
    while IFS= read -r -d '' f; do
      convert_one "$f"
      count=$((count + 1))
    done < <(find "$item" -maxdepth 1 -type f \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' -o -iname '*.heic' \) -print0 | sort -z)
  elif [ -f "$item" ]; then
    convert_one "$item"
    count=$((count + 1))
  else
    echo "skipping $item (not found)" >&2
  fi
done
echo "Done: $count image(s) in $outdir – upload the .bmp and .bmp.cfg files together."
