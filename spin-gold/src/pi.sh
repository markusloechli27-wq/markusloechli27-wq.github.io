#!/bin/sh
# Holt das Flop-Programm für die Poker-Review-App nach ~/flops und startet es.
#   wget -qO- https://markusloechli27-wq.github.io/spin-gold/src/pi.sh | sh          (Anzahl Rechnungen automatisch)
#   wget -qO- https://markusloechli27-wq.github.io/spin-gold/src/pi.sh | sh -s 2     (2 Rechnungen)
# The ?t= suffix skips the 10-minute cache of GitHub Pages, so a fresh version arrives right after an update.
B=https://markusloechli27-wq.github.io/spin-gold/src
mkdir -p "$HOME/flops" && cd "$HOME/flops" || exit 1
for f in pfsolve.c pflib.c pi-rechnen.sh pi-auftrag.txt; do
  wget -qO "$f.neu" "$B/$f?t=$(date +%s)" && mv "$f.neu" "$f" || { rm -f "$f.neu"; echo "Download fehlgeschlagen: $f"; exit 1; }
done
echo "Programm liegt in $HOME/flops"
sh pi-rechnen.sh "$@"
