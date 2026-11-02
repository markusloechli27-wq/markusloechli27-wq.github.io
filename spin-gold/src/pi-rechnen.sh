#!/bin/sh
# Flop-Bibliothek auf dem Raspberry Pi rechnen
# ---------------------------------------------
# Einrichten und starten (ein Befehl, im Terminal auf dem Pi):
#   mkdir -p ~/flops && cd ~/flops && for f in pfsolve.c pflib.c pi-rechnen.sh pi-auftrag.txt; do wget -qO $f https://markusloechli27-wq.github.io/spin-gold/src/$f; done && sh pi-rechnen.sh 4
#
#   Die Zahl am Ende ist die Anzahl paralleler Rechnungen. Jede braucht bis etwa 600 MB RAM:
#   4 bei 8 GB oder 16 GB, 2 bei 4 GB. Der Pi sollte einen Lüfter haben, sonst drosselt er unter Dauerlast.
#   Es läuft im Hintergrund weiter, auch wenn du das Terminal schließt, und macht nach einem Neustart
#   (gleicher Befehl) dort weiter, wo es aufgehört hat.
#
# Fortschritt ansehen:        cd ~/flops && tail -f log-0.txt        (beenden mit Strg+C, das Rechnen läuft weiter)
# Ergebnisse aufs iPhone:     cd ~/flops && sh pi-rechnen.sh senden
#                             zeigt eine Adresse, die du im iPhone-Browser öffnest (gleiches WLAN). Die Datei landet in
#                             „Downloads“; in der App unter Solver > Flop-Bibliothek > „Importieren“ auswählen.
#                             Es kommen nur die Flops, die seit dem letzten Senden neu sind. Alles nochmal: sh pi-rechnen.sh senden alles
# Stoppen:                    pkill pflib
cd "$(dirname "$0")" || exit 1

if [ "$1" = "senden" ]; then
  [ "$2" = "alles" ] && rm -f .gesendet
  out="neu-$(date +%Y%m%d-%H%M).jsonl"; : > "$out"; n=0
  for f in ergebnis-*.jsonl; do
    [ -f "$f" ] || continue
    old=$(awk -v f="$f" '$1 == f { print $2 }' .gesendet 2>/dev/null); old=${old:-0}
    total=$(wc -l < "$f")                       # nur vollständige Zeilen
    if [ "$total" -gt "$old" ]; then head -n "$total" "$f" | tail -n +"$((old + 1))" >> "$out"; n=$((n + total - old)); fi
    { grep -v "^$f " .gesendet 2>/dev/null; echo "$f $total"; } > .gesendet.neu && mv .gesendet.neu .gesendet
  done
  if [ "$n" -eq 0 ]; then rm -f "$out"; echo "Seit dem letzten Senden ist nichts Neues fertig geworden."; exit 0; fi
  ip=$(hostname -I | awk '{ print $1 }')
  echo "$n neue Flops in $out"
  echo "Im iPhone-Browser öffnen:   http://$ip:8000/$out"
  echo "Dann in der App unter „Importieren“ aus „Downloads“ auswählen. Danach hier Strg+C drücken."
  python3 -c "
import http.server, functools
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if self.path.endswith('.jsonl'): self.send_header('Content-Disposition', 'attachment')
        super().end_headers()
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(('', 8000), H).serve_forever()" 2>/dev/null
  exit 0
fi

N=${1:-4}
if pgrep -x pflib > /dev/null; then echo "Läuft schon ($(pgrep -xc pflib) Rechnungen). Fortschritt: tail -f log-0.txt"; exit 0; fi
[ -f pi-auftrag.txt ] || { echo "pi-auftrag.txt fehlt in $(pwd)"; exit 1; }
gcc -O3 -march=native -o pflib pflib.c -lm 2>/dev/null || { echo "Kompilieren fehlgeschlagen. Einmal ausführen: sudo apt install -y build-essential"; exit 1; }
i=0
while [ $i -lt "$N" ]; do
  nohup ./pflib pi-auftrag.txt $i "$N" ergebnis-$i.jsonl ergebnis-*.jsonl > log-$i.txt 2>&1 &
  i=$((i + 1))
done
echo "$N Rechnungen gestartet. Fortschritt: tail -f log-0.txt"
