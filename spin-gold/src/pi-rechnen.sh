#!/bin/sh
# Flop-Bibliothek auf dem Raspberry Pi rechnen
# ---------------------------------------------
# Einrichten und starten (ein Befehl, im Terminal auf dem Pi):
#   wget -qO- https://markusloechli27-wq.github.io/spin-gold/src/pi.sh | sh
#
#   Die Zahl paralleler Rechnungen (bis 4) wählt es selbst nach freiem RAM; jede braucht bis etwa 600 MB.
#   Selbst festlegen: … | sh -s 2. Die Rechnungen laufen mit niedrigster Priorität, andere Dienste auf dem Pi
#   (z. B. Jarvis) haben immer Vorrang. Der Pi sollte einen Lüfter haben, sonst drosselt er unter Dauerlast.
#   Es läuft im Hintergrund weiter, auch wenn du das Terminal schließt; nach einem Neustart des Pi denselben
#   Befehl nochmal, dann macht es dort weiter, wo es aufgehört hat.
#
# Stand ansehen:              cd ~/flops && sh pi-rechnen.sh status
# Fortschritt ansehen:        cd ~/flops && tail -f log-0.txt        (beenden mit Strg+C, das Rechnen läuft weiter)
# Ergebnisse aufs iPhone:     cd ~/flops && sh pi-rechnen.sh senden
#                             zeigt eine Adresse, die du im iPhone-Browser öffnest (über Tailscale auch unterwegs). Die Datei landet in
#                             „Downloads“; in der App unter Solver > Flop-Bibliothek > „Importieren“ auswählen.
#                             Es kommen nur die Flops, die seit dem letzten Senden neu sind. Alles nochmal: sh pi-rechnen.sh senden alles
# Stoppen:                    pkill pflib
cd "$(dirname "$0")" || exit 1

if [ "$1" = "status" ]; then
  echo "Rechnungen aktiv: $(pgrep -xc pflib)"
  echo "Fertige Flops: $(cat ergebnis-*.jsonl 2>/dev/null | wc -l) von $(grep -c '^F ' pi-auftrag.txt 2>/dev/null)"
  command -v vcgencmd > /dev/null && vcgencmd measure_temp 2>/dev/null
  free -h | head -2
  for f in log-*.txt; do [ -f "$f" ] && tail -n 1 "$f"; done
  exit 0
fi

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
  P=8731
  echo "$n neue Flops in $out"
  echo "Im iPhone-Browser öffnen:"
  for ip in $(hostname -I); do case "$ip" in *:*) ;; 100.*) echo "   http://$ip:$P/$out   (Tailscale, geht auch unterwegs)";; *) echo "   http://$ip:$P/$out   (nur im gleichen WLAN)";; esac; done
  echo "Dann in der App unter „Importieren“ aus „Downloads“ auswählen. Danach hier Strg+C drücken."
  python3 -c "
import http.server
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if self.path.endswith('.jsonl'): self.send_header('Content-Disposition', 'attachment')
        super().end_headers()
    def log_message(self, *a): pass
try: http.server.ThreadingHTTPServer(('', $P), H).serve_forever()
except KeyboardInterrupt: pass
except OSError as e: print('Server startet nicht:', e)"
  exit 0
fi

if pgrep -x pflib > /dev/null; then echo "Läuft schon ($(pgrep -xc pflib) Rechnungen). Stand: sh pi-rechnen.sh status"; exit 0; fi
if [ -n "$1" ]; then N=$1; else
  avail=$(awk '/MemAvailable/ { print int($2 / 1024) }' /proc/meminfo); cores=$(nproc)
  N=$(( (avail - 800) / 650 )); [ "$N" -gt "$cores" ] && N=$cores; [ "$N" -gt 4 ] && N=4; [ "$N" -lt 1 ] && N=1
  echo "Freier RAM: $avail MB, also $N parallele Rechnungen."
fi
[ -f pi-auftrag.txt ] || { echo "pi-auftrag.txt fehlt in $(pwd)"; exit 1; }
gcc -O3 -march=native -o pflib pflib.c -lm 2>/dev/null || { echo "Kompilieren fehlgeschlagen. Einmal ausführen: sudo apt install -y build-essential"; exit 1; }
i=0
while [ $i -lt "$N" ]; do
  # lowest CPU priority; if memory ever runs out, the kernel stops the solver first, never Jarvis or other services
  nohup sh -c 'echo 1000 > /proc/self/oom_score_adj 2>/dev/null; exec nice -n 19 ./pflib "$@"' pflib pi-auftrag.txt $i "$N" ergebnis-$i.jsonl ergebnis-*.jsonl > log-$i.txt 2>&1 &
  i=$((i + 1))
done
echo "$N Rechnungen gestartet. Stand ansehen: cd ~/flops && sh pi-rechnen.sh status"
