#!/usr/bin/env python3
# Spin & Gold preflop solves on the Raspberry Pi for the poker review app.
# Takes its task list from GitHub Pages (spin-auftrag.json), solves one stack combination after the other with mr2
# and appends each result as one line to ergebnis.jsonl. Waits while there is no task list yet, and checks
# for new tasks when everything is done, so new work arrives without a new command on the Pi.
import json, subprocess, base64, os, time, urllib.request, hashlib
B = os.environ.get('SPIN_SRC', 'https://markusloechli27-wq.github.io/spin-gold/src/')
D = os.path.dirname(os.path.abspath(__file__)); os.chdir(D)
MODELS = ['cev', 'x10', 'x50', 'jp']

def log(*a): print(time.strftime('%d.%m. %H:%M'), *a, flush=True)
def get(name):
    with urllib.request.urlopen(B + name + '?t=%d' % time.time(), timeout=120) as r: return r.read()

def pack(nodes, keep):  # 4-bit frequencies of the displayed spots, same format as the app
    out = {}
    for n in nodes:
        if n['path'] not in keep: continue
        acts = [a for a in n['acts'] if a not in ('fold', 'check')]
        vals = [min(15, max(0, round(v / 1000 * 15))) for a in acts for v in n['f'][n['acts'].index(a)]]
        b = bytes((vals[i] | (vals[i + 1] << 4)) if i + 1 < len(vals) else vals[i] for i in range(0, len(vals), 2))
        out[n['path']] = {'p': n['p'], 'acts': acts, 'rest': 'check' if 'check' in n['acts'] else 'fold', 'f': base64.b64encode(b).decode()}
    return out

def done_keys():
    s = set()
    if os.path.exists('ergebnis.jsonl'):
        for line in open('ergebnis.jsonl'):
            try: o = json.loads(line); s.add((o['v'], o['m'], o['k']))
            except Exception: pass          # half-written last line after a power cut
    return s

def prepare(job):  # program and calibration of this job version
    if not os.path.exists('.mr2ver') or open('.mr2ver').read() != job['mr2']:
        for f in ('mr2.c', 'pk.h', 'eq.bin', 'p3.bin'):
            data = get(f)
            if f == 'mr2.c' and hashlib.sha1(data).hexdigest()[:12] != job['mr2']: raise RuntimeError('mr2.c noch nicht aktuell (Cache)')
            open(f, 'wb').write(data)
        r = subprocess.run(['gcc', '-O3', '-march=native', '-fopenmp', '-o', 'mr2', 'mr2.c', '-lm'], capture_output=True, text=True)
        if r.returncode: raise RuntimeError('Kompilieren fehlgeschlagen: ' + r.stderr[-300:])
        open('.mr2ver', 'w').write(job['mr2']); log('Programm', job['mr2'], 'bereit')
    if not os.path.exists('.eqrver') or open('.eqrver').read() != job['v']:
        open('eqr.txt', 'wb').write(get('spin-eqr.txt'))
        if hashlib.sha1(open('eqr.txt', 'rb').read()).hexdigest()[:12] != job['eqr']: raise RuntimeError('eqr.txt noch nicht aktuell (Cache)')
        open('.eqrver', 'w').write(job['v']); log('Eichung', job['v'], 'bereit')

while True:
    try: job = json.loads(get('spin-auftrag.json'))
    except Exception as e: log('Auftrag nicht erreichbar:', e); time.sleep(300); continue
    if not job.get('tasks'): log('Warte auf Aufgaben:', job.get('info', '')); time.sleep(600); continue
    try: prepare(job)
    except Exception as e: log(e); time.sleep(300); continue
    v, done = job['v'], done_keys()
    todo = [t for t in job['tasks'] if (v, t[0], t[1]) not in done]
    if not todo: log('Alles fertig: %d Lösungen. Warte auf neue Aufgaben.' % len(job['tasks'])); time.sleep(1800); continue
    for m, k in todo[:5]:                   # look at the task list again after a few solves
        t0 = time.time(); st = k.replace('|', ',')
        r = subprocess.run(['./mr2', '3h', st.split(',')[0], str(job['iters']), '0'], capture_output=True, text=True,
                           env=dict(os.environ, STACKS=st, MODEL=str(MODELS.index(m))))
        if r.returncode: log('Fehler bei', m, k, r.stderr[-200:]); time.sleep(60); break
        ex = r.stderr.strip().split('\n')[-2].strip()
        line = json.dumps({'v': v, 'm': m, 'k': k, 'ex': ex, 'd': pack(json.loads(r.stdout)['nodes'], set(job['spots']))}, separators=(',', ':'))
        with open('ergebnis.jsonl', 'a') as f: f.write(line + '\n')
        log('%s %s fertig in %.0f s (%d von %d)' % (m, k, time.time() - t0, len(done_keys() & {(v, a, b) for a, b in job['tasks']}), len(job['tasks'])))
