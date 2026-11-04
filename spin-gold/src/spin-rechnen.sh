#!/bin/sh
# Spin & Gold auf dem Raspberry Pi rechnen (Preflop-Lösungen für alle Stack-Kombinationen und Multiplikatoren)
# --------------------------------------------------------------------------------------------------------
# Einrichten und starten (ein Befehl, im Terminal auf dem Pi):
#   wget -qO- https://markusloechli27-wq.github.io/spin-gold/src/spin.sh | sh
#   Stoppt dabei die Cash-Flop-Bibliothek und richtet Spin & Gold als Dienst ein (läuft weiter ohne Verbindung,
#   startet nach einem Neustart von selbst). Die Aufgaben holt sich der Pi selbst; neue kommen ohne neuen Befehl.
#
# Stand ansehen:      cd ~/spin && sh spin-rechnen.sh status
# Ergebnis senden:    cd ~/spin && sh spin-rechnen.sh senden
#                     zeigt eine Adresse für den iPhone-Browser; die Datei landet in „Downloads“.
#                     Diese Datei dann im Chat mit Claude anhängen.
# Stoppen:            cd ~/spin && sh spin-rechnen.sh stopp
cd "$(dirname "$0")" || exit 1
DIR=$(pwd)

if [ "$1" = "status" ]; then
  echo "Dienst: $(systemctl is-active spin-gold 2>/dev/null)"
  echo "Fertige Lösungen: $(cat ergebnis.jsonl 2>/dev/null | wc -l)"
  command -v vcgencmd > /dev/null && vcgencmd measure_temp 2>/dev/null
  free -h | head -2
  tail -n 3 log.txt 2>/dev/null
  exit 0
fi

if [ "$1" = "stopp" ]; then
  sudo systemctl disable --now spin-gold > /dev/null 2>&1; pkill -f spin-pi.py 2> /dev/null; pkill -x mr2 2> /dev/null
  echo "Gestoppt. Fertige Lösungen bleiben erhalten; neu starten mit: sh spin-rechnen.sh"
  exit 0
fi

if [ "$1" = "senden" ]; then
  [ -s ergebnis.jsonl ] || { echo "Noch keine Lösung fertig."; exit 0; }
  out="spin-ergebnis-$(date +%Y%m%d-%H%M).txt"; cp ergebnis.jsonl "$out"
  P=8731
  echo "$(wc -l < "$out") Lösungen in $out"
  echo "Im iPhone-Browser öffnen:"
  for ip in $(hostname -I); do case "$ip" in *:*) ;; 100.*) echo "   http://$ip:$P/$out   (Tailscale, geht auch unterwegs)";; *) echo "   http://$ip:$P/$out   (nur im gleichen WLAN)";; esac; done
  echo "Die Datei landet in „Downloads“. Hänge sie im Chat mit Claude an. Danach hier Strg+C drücken."
  python3 -c "
import http.server
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if self.path.endswith('.txt'): self.send_header('Content-Disposition', 'attachment')
        super().end_headers()
    def log_message(self, *a): pass
try: http.server.ThreadingHTTPServer(('', $P), H).serve_forever()
except KeyboardInterrupt: pass
except OSError as e: print('Server startet nicht:', e)"
  exit 0
fi

# ---- starten ----
command -v gcc > /dev/null || { echo "gcc fehlt. Einmal ausführen: sudo apt install -y build-essential"; exit 1; }
command -v python3 > /dev/null || { echo "python3 fehlt. Einmal ausführen: sudo apt install -y python3"; exit 1; }
ME=$(id -un)
echo "Einmal dein Passwort, damit Spin & Gold als Dienst läuft (wie Jarvis):"
# Cash-Flop-Bibliothek anhalten (fertige Flops bleiben in ~/flops erhalten)
for u in $(systemctl list-units --all --plain --no-legend 'flops@*' 2>/dev/null | awk '{ print $1 }'); do sudo systemctl disable --now "$u" > /dev/null 2>&1; done
pkill -x pflib 2> /dev/null
printf '%s\n' "[Unit]
Description=Spin & Gold Preflop-Lösungen Poker Review
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$ME
WorkingDirectory=$DIR
ExecStart=/usr/bin/python3 -u $DIR/spin-pi.py
Nice=19
IOSchedulingClass=idle
OOMScoreAdjust=1000
Restart=always
RestartSec=120
StandardOutput=append:$DIR/log.txt
StandardError=append:$DIR/log.txt

[Install]
WantedBy=multi-user.target" | sudo tee /etc/systemd/system/spin-gold.service > /dev/null || { echo "Ohne Passwort geht es nicht. Nochmal: sh spin-rechnen.sh"; exit 1; }
sudo systemctl daemon-reload
sudo systemctl enable spin-gold > /dev/null 2>&1
sudo systemctl restart spin-gold || { echo "Dienst startet nicht: sudo systemctl status spin-gold"; exit 1; }
sleep 3
echo "Cash-Bibliothek gestoppt. Spin & Gold läuft als Dienst ($(systemctl is-active spin-gold))."
echo "Stand ansehen: cd ~/spin && sh spin-rechnen.sh status"
tail -n 2 log.txt 2>/dev/null
