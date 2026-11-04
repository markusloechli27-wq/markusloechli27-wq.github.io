#!/bin/sh
# Holt das Spin-&-Gold-Programm nach ~/spin, stoppt die Cash-Flop-Bibliothek und startet Spin & Gold als Dienst.
#   wget -qO- https://markusloechli27-wq.github.io/spin-gold/src/spin.sh | sh
# The ?t= suffix skips the 10-minute cache of GitHub Pages.
B=https://markusloechli27-wq.github.io/spin-gold/src
mkdir -p "$HOME/spin" && cd "$HOME/spin" || exit 1
for f in spin-pi.py spin-rechnen.sh; do
  wget -qO "$f.neu" "$B/$f?t=$(date +%s)" && mv "$f.neu" "$f" || { rm -f "$f.neu"; echo "Download fehlgeschlagen: $f"; exit 1; }
done
echo "Programm liegt in $HOME/spin"
sh spin-rechnen.sh "$@"
