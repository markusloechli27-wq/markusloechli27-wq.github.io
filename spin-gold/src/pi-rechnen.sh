#!/bin/sh
# Flop-Bibliothek auf dem Raspberry Pi rechnen
# ---------------------------------------------
# 1. In der App (Solver > Flop-Bibliothek) „Pi-Auftrag erstellen“ und „Pi-Programm laden“ tippen.
#    Die vier Dateien (pi-auftrag.txt, pfsolve.c, pflib.c, pi-rechnen.sh) in einen Ordner auf dem Pi kopieren, z. B. ~/flops
# 2. Auf dem Pi im Terminal:   cd ~/flops && sh pi-rechnen.sh 4
#    Die Zahl ist die Anzahl paralleler Rechnungen: 4 bei 8 GB oder 16 GB RAM, 2 bei 4 GB.
# 3. Es läuft im Hintergrund weiter, auch wenn du das Terminal schließt. Fortschritt:   tail -f log-0.txt
# 4. Übertragen aufs iPhone: auf dem Pi   python3 -m http.server 8000   starten, die IP steht bei   hostname -I
#    Im iPhone-Browser http://<IP>:8000 öffnen, die Dateien ergebnis-*.jsonl laden und in der App unter „Importieren“ auswählen.
# Nochmal starten macht dort weiter, wo es aufgehört hat. Stoppen:   pkill pflib
N=${1:-4}
gcc -O3 -march=native -o pflib pflib.c -lm || { echo "Kompilieren fehlgeschlagen (sudo apt install build-essential)"; exit 1; }
i=0
while [ $i -lt $N ]; do
  nohup ./pflib pi-auftrag.txt $i $N ergebnis-$i.jsonl > log-$i.txt 2>&1 &
  i=$((i+1))
done
echo "$N Rechnungen gestartet. Fortschritt: tail -f log-0.txt"
