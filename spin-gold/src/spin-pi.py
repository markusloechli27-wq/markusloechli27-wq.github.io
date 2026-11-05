#!/usr/bin/env python3
# Spin & Gold on the Raspberry Pi for the poker review app.
# Takes its work from GitHub Pages (spin-auftrag.json): preflop solves with mr2 (one stack combination each,
# results in ergebnis.jsonl), then flop solves with cbet (one flop of one spot each, results in cb-ergebnis.jsonl),
# then the less urgent preflop solves in 'late' (finer ChipEV grid, payout models with prizes for 2nd and 3rd).
# Waits while there is nothing to do and checks for new work, so new tasks arrive without a new command on the Pi.
# Updates itself: when the job names a newer version of this file, it downloads it and restarts (systemd restarts it).
import json, subprocess, base64, os, sys, time, urllib.request, hashlib
B = os.environ.get('SPIN_SRC', 'https://markusloechli27-wq.github.io/spin-gold/src/')
D = os.path.dirname(os.path.abspath(__file__)); os.chdir(D)
MODELS = ['cev', 'x10', 'x50', 'jp']
sha = lambda b: hashlib.sha1(b).hexdigest()[:12]

def log(*a): print(time.strftime('%d.%m. %H:%M'), *a, flush=True)
def get(name):
    with urllib.request.urlopen(B + name + '?t=%d' % time.time(), timeout=120) as r: return r.read()
def fetch(name, want):  # download a file and check that it is the version the job names (GitHub Pages caches for a while)
    data = get(name)
    if want and sha(data) != want: raise RuntimeError(name + ' noch nicht aktuell (Cache), nächster Versuch in 5 Minuten')
    open(name, 'wb').write(data)

def pack(nodes, keep):  # 4-bit frequencies of the displayed spots, same format as the app
    out = {}
    for n in nodes:
        if n['path'] not in keep: continue
        acts = [a for a in n['acts'] if a not in ('fold', 'check')]
        vals = [min(15, max(0, round(v / 1000 * 15))) for a in acts for v in n['f'][n['acts'].index(a)]]
        b = bytes((vals[i] | (vals[i + 1] << 4)) if i + 1 < len(vals) else vals[i] for i in range(0, len(vals), 2))
        out[n['path']] = {'p': n['p'], 'acts': acts, 'rest': 'check' if 'check' in n['acts'] else 'fold', 'f': base64.b64encode(b).decode()}
    return out

def keys(fn, f):
    s = set()
    if os.path.exists(fn):
        for line in open(fn):
            try: s.add(f(json.loads(line)))
            except Exception: pass          # half-written last line after a power cut
    return s
def mem_free_mb():
    for l in open('/proc/meminfo'):
        if l.startswith('MemAvailable'): return int(l.split()[1]) // 1024
    return 0

def prepare(job):  # programs and calibration of this job version
    if not os.path.exists('.mr2ver') or open('.mr2ver').read() != job['mr2']:
        fetch('mr2.c', job['mr2']); fetch('pk.h', None); fetch('eq.bin', None); fetch('p3.bin', None)
        r = subprocess.run(['gcc', '-O3', '-march=native', '-fopenmp', '-o', 'mr2', 'mr2.c', '-lm'], capture_output=True, text=True)
        if r.returncode: raise RuntimeError('Kompilieren fehlgeschlagen: ' + r.stderr[-300:])
        open('.mr2ver', 'w').write(job['mr2']); log('Preflop-Programm', job['mr2'], 'bereit')
    if not os.path.exists('.eqrver') or open('.eqrver').read() != job['v']:
        data = get('spin-eqr.txt')
        if sha(data) != job['eqr']: raise RuntimeError('eqr.txt noch nicht aktuell (Cache)')
        open('eqr.txt', 'wb').write(data); open('.eqrver', 'w').write(job['v']); log('Eichung', job['v'], 'bereit')
    cb = job.get('cb')
    cbkey = cb and '|'.join((cb['v'], cb.get('cbet', ''), cb.get('pfsolve', '')))   # a faster program with the same results keeps cb['v']
    if cb and (not os.path.exists('.cbver') or open('.cbver').read() != cbkey):
        fetch('cbet.c', cb['cbet']); fetch('pfsolve.c', cb['pfsolve']); fetch('spin-cb-ranges.json', cb['ranges'])
        r = subprocess.run(['gcc', '-O3', '-march=native', '-o', 'cbet', 'cbet.c', '-lm'], capture_output=True, text=True)
        if r.returncode: raise RuntimeError('Kompilieren fehlgeschlagen: ' + r.stderr[-300:])
        open('.cbver', 'w').write(cbkey); log('Flop-Programm', cb['v'], cb.get('pfsolve', ''), 'bereit')

def preflop(job, m, k):
    t0 = time.time(); st = k.replace('|', ',')
    r = subprocess.run(['./mr2', '3h', st.split(',')[0], str(job['iters']), '0'], capture_output=True, text=True,
                       env=dict(os.environ, STACKS=st, MODEL=str(MODELS.index(m))))
    if r.returncode: log('Fehler bei', m, k, r.stderr[-200:]); return False
    ex = r.stderr.strip().split('\n')[-2].strip()
    line = json.dumps({'v': job['v'], 'm': m, 'k': k, 'ex': ex, 'd': pack(json.loads(r.stdout)['nodes'], set(job['spots']))}, separators=(',', ':'))
    with open('ergebnis.jsonl', 'a') as f: f.write(line + '\n')
    allt = job['tasks'] + job.get('late', [])
    done = len(keys('ergebnis.jsonl', lambda o: (o['v'], o['m'], o['k'])) & {(job['v'], a, b) for a, b in allt})
    log('Preflop %s %s fertig in %.0f s (%d von %d)' % (m, k, time.time() - t0, done, len(allt))); return True

def flop(cb, t):
    sid, fl, pot, stack, env, mb = t
    if mem_free_mb() < mb + 300: log('Warte auf Speicher für %s %s (%d MB nötig, %d frei)' % (sid, fl, mb + 300, mem_free_mb())); time.sleep(300); return False
    t0 = time.time(); open('fl_one.txt', 'w').write(fl + ' 1\n')
    r = subprocess.run(['./cbet', 'spin-cb-ranges.json', sid + '_oop', sid + '_ip', str(pot), str(stack), 'fl_one.txt', '1', '0', '1'], capture_output=True, text=True,
                       env=dict(os.environ, PF_HEAP_MB=str(mb), **env))
    if r.returncode or not r.stdout.strip(): log('Fehler bei', sid, fl, r.returncode, r.stderr[-200:]); time.sleep(60); return False
    with open('cb-ergebnis.jsonl', 'a') as f: f.write(json.dumps({'v': cb['v'], 'sid': sid, 'r': json.loads(r.stdout)}, separators=(',', ':')) + '\n')
    done = len(keys('cb-ergebnis.jsonl', lambda o: (o['v'], o['sid'], o['r']['flop'])) & {(cb['v'], a[0], a[1]) for a in cb['tasks']})
    log('Flop %s %s fertig in %.0f s (%d von %d)' % (sid, fl, time.time() - t0, done, len(cb['tasks']))); return True

while True:
    try: job = json.loads(get('spin-auftrag.json'))
    except Exception as e: log('Auftrag nicht erreichbar:', e); time.sleep(300); continue
    if job.get('worker') and job['worker'] != sha(open(__file__, 'rb').read()):
        try:
            for f, w in (('spin-pi.py', job['worker']), ('spin-rechnen.sh', job.get('script'))): fetch(f, w)
            log('Neue Version geladen, starte neu'); sys.exit(0)
        except Exception as e: log(e); time.sleep(300); continue
    if not job.get('tasks') and not job.get('cb') and not job.get('late'): log('Warte auf Aufgaben:', job.get('info', '')); time.sleep(600); continue
    try: prepare(job)
    except Exception as e: log(e); time.sleep(300); continue
    v = job['v']; done = keys('ergebnis.jsonl', lambda o: (o['v'], o['m'], o['k']))
    todo = [t for t in job.get('tasks', []) if (v, t[0], t[1]) not in done]
    if todo:
        for m, k in todo[:5]:               # look at the job again after a few solves
            if not preflop(job, m, k): time.sleep(60); break
        continue
    cb = job.get('cb')
    if cb:
        cdone = keys('cb-ergebnis.jsonl', lambda o: (o['v'], o['sid'], o['r']['flop']))
        ctodo = [t for t in cb['tasks'] if (cb['v'], t[0], t[1]) not in cdone]
        if ctodo: flop(cb, ctodo[0]); continue
    late = [t for t in job.get('late', []) if (v, t[0], t[1]) not in done]   # after the flops
    if late:
        for m, k in late[:5]:
            if not preflop(job, m, k): time.sleep(60); break
        continue
    log('Alles fertig. Warte auf neue Aufgaben.'); time.sleep(1800)
