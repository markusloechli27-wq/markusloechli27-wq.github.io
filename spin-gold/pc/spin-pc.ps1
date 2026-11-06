# Spin & Gold: Rechner für einen Windows-PC.
# Holt seine Aufgaben von GitHub Pages (spin-auftrag-pc.json), rechnet sie mit mehreren Kernen gleichzeitig und
# schreibt die Ergebnisse nach ergebnis-pc.jsonl in diesem Ordner. Erst die Preflop-Lösungen (mr2), dann Flops (cbet).
# Abbrechen geht jederzeit (Fenster schließen); beim nächsten Start geht es dort weiter, wo es aufgehört hat.
# Läuft nie neben GGPoker: Ist der GG-Client offen, startet das Skript nicht; wird er geöffnet, beendet es alle
# Rechnungen sofort und sich selbst.
param([int]$Kerne = 0, [int]$SpeicherProzent = 70, [int]$Max = 0, [switch]$NurFlops)   # Max: stop after that many results (test)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$B = 'https://markusloechli27-wq.github.io/spin-gold/'
$D = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $D
$OUT = Join-Path $D 'ergebnis-pc-v2.jsonl'   # v2: the first version lost the solver settings on the way to the programs
$UTF8 = New-Object Text.UTF8Encoding $false   # no byte order mark: every line must be plain JSON
$TMP = Join-Path $D 'tmp'; New-Item -ItemType Directory -Force -Path $TMP | Out-Null
[Environment]::CurrentDirectory = $D

function Log($m) { $l = (Get-Date -Format 'dd.MM. HH:mm:ss') + '  ' + $m; Write-Host $l; Add-Content -Path (Join-Path $D 'log.txt') -Value $l -Encoding UTF8 }
function Sha12($f) { (Get-FileHash -Algorithm SHA1 -Path $f).Hash.ToLower().Substring(0, 12) }
function Url($p) { $B + $p + '?t=' + [DateTimeOffset]::Now.ToUnixTimeSeconds() }
function Fetch($p, $file, $want) {   # download and, if a version is named, check it (GitHub Pages caches for a while)
  $f = Join-Path $D $file
  if ((Test-Path $f) -and (-not $want -or (Sha12 $f) -eq $want)) { return }
  Invoke-WebRequest -UseBasicParsing -Uri (Url $p) -OutFile $f -TimeoutSec 600
  if ($want -and (Sha12 $f) -ne $want) { throw "$file ist auf GitHub noch nicht aktuell (Cache). Nächster Versuch in 5 Minuten." }
}

# ---- never next to GGPoker ----
$GG = '(?i)^gg|ggnet|ggpoker|pokercraft|nsus'
function GGOffen { [bool](Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match $GG }) }
$running = New-Object System.Collections.ArrayList
function AllesStoppen { foreach ($r in $running) { try { if (-not $r.p.HasExited) { $r.p.Kill() } } catch {} }; $running.Clear()
  Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(mr2|cbet)-x86' } | Stop-Process -Force -ErrorAction SilentlyContinue }
# the solver processes die with this window (a Windows job object), so none keeps running unseen
try {
  Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class SpinJob {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr a, string n);
  [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr j, int c, IntPtr i, uint l);
  [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr j, IntPtr p);
  [StructLayout(LayoutKind.Sequential)] struct BASIC { public long a, b; public uint LimitFlags; public UIntPtr c, d; public uint e; public UIntPtr f; public uint g, h; }
  [StructLayout(LayoutKind.Sequential)] struct IOC { public ulong a, b, c, d, e, f; }
  [StructLayout(LayoutKind.Sequential)] struct EXT { public BASIC B; public IOC I; public UIntPtr p, j, pu, ju; }
  static IntPtr H;
  public static bool Init() { H = CreateJobObject(IntPtr.Zero, null); EXT x = new EXT(); x.B.LimitFlags = 0x2000;
    int n = Marshal.SizeOf(typeof(EXT)); IntPtr p = Marshal.AllocHGlobal(n); Marshal.StructureToPtr(x, p, false); return SetInformationJobObject(H, 9, p, (uint)n); }
  public static bool Add(IntPtr proc) { return AssignProcessToJobObject(H, proc); }
  [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint f);
  public static void Wach() { SetThreadExecutionState(0x80000001); }   // the PC does not go to sleep while this window is open
}
"@
  $jobOk = [SpinJob]::Init(); [SpinJob]::Wach()
} catch { $jobOk = $false }
AllesStoppen   # leftovers of an earlier run
if (GGOffen) { Log 'GGPoker ist offen. Der Rechner startet nicht neben dem GG-Client. Bitte GG schließen und neu starten.'; Read-Host 'Enter zum Beenden'; exit 1 }

# ---- machine ----
$cs = Get-CimInstance Win32_ComputerSystem
$ramMB = [int]($cs.TotalPhysicalMemory / 1MB)
$cores = [int]$cs.NumberOfLogicalProcessors
if ($Kerne -le 0) { $Kerne = [Math]::Max(1, $cores - 1) }   # one core stays free for the PC itself
$budget = [int]($ramMB * $SpeicherProzent / 100)
function FreiMB { [int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1KB) }
Log "PC: $cores Kerne, $ramMB MB Speicher. Rechne mit $Kerne Kernen, höchstens $budget MB."

# ---- 4-bit packing, same format as the app and the Pi ----
function R15([double]$v) { [int][Math]::Min(15, [Math]::Max(0, [Math]::Round($v / 1000 * 15))) }
function Q4([int[]]$vals) {
  $b = New-Object byte[] ([Math]::Ceiling($vals.Count / 2))
  for ($i = 0; $i -lt $vals.Count; $i += 2) { $x = $vals[$i]; if ($i + 1 -lt $vals.Count) { $x = $x -bor ($vals[$i + 1] -shl 4) }; $b[[int]($i / 2)] = [byte]$x }
  [Convert]::ToBase64String($b)
}
function Pack($nodes, $keep, [bool]$patched) {   # patched: lines behind a patch (with the actor's reach) and every node the base leaves out
  $out = [ordered]@{}
  foreach ($n in $nodes) {
    $hasLk = $n.PSObject.Properties.Name -contains 'lk'; $inKeep = $keep.Contains([string]$n.path)
    $want = if ($patched) { $hasLk -or -not $inKeep } else { $inKeep -and -not $hasLk }
    if (-not $want) { continue }
    $acts = New-Object System.Collections.Generic.List[string]; $vals = New-Object System.Collections.Generic.List[int]
    for ($i = 0; $i -lt $n.acts.Count; $i++) { $a = [string]$n.acts[$i]; if ($a -eq 'fold' -or $a -eq 'check') { continue }
      $acts.Add($a); foreach ($v in $n.f[$i]) { $vals.Add((R15 $v)) } }
    $o = [ordered]@{ p = [int]$n.p; acts = [string[]]$acts.ToArray(); rest = $(if (@($n.acts) -contains 'check') { 'check' } else { 'fold' }); f = (Q4 $vals.ToArray()) }
    if ($patched -and $hasLk) { $o.lk = [string]$n.lk; $o.r = Q4 ([int[]]@($n.r | ForEach-Object { R15 $_ })) }
    $out[[string]$n.path] = $o
  }
  $out
}
function J($o) { ConvertTo-Json $o -Depth 12 -Compress }

# ---- what is done already ----
$done = New-Object 'System.Collections.Generic.HashSet[string]'
if (Test-Path $OUT) { foreach ($l in [IO.File]::ReadLines($OUT)) { try { $o = $l | ConvertFrom-Json
  if ($o.PSObject.Properties.Name -contains 'sid') { [void]$done.Add("$($o.v)|$($o.sid)|$($o.r.flop)") } else { [void]$done.Add("$($o.v)|$($o.m)|$($o.k)") } } catch {} } }

# ---- programs: the faster build when this CPU has AVX2 ----
$job = $null; $exeM = $null; $exeC = $null
function Vorbereiten {
  $script:job = Invoke-RestMethod -UseBasicParsing -Uri (Url 'src/spin-auftrag-pc.json') -TimeoutSec 120
  if ($script:job.worker -and (Sha12 $PSCommandPath) -ne $script:job.worker) {   # a newer version of this script: load it and start it again
    Fetch 'pc/spin-pc.ps1' 'spin-pc.ps1.neu' $script:job.worker; Move-Item -Force (Join-Path $D 'spin-pc.ps1.neu') $PSCommandPath
    Log 'Neue Version geladen, starte neu.'; Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"" -WorkingDirectory $D; exit 0 }
  $bins = $script:job.bins
  foreach ($f in 'Ergebnis-einpacken.bat', 'Rechnen-starten.bat', 'LIESMICH.txt') {   # helper files follow the worker version
    try { Invoke-WebRequest -UseBasicParsing -Uri (Url "pc/$f") -OutFile (Join-Path $D $f) -TimeoutSec 60 } catch {} }
  foreach ($a in 'x86-64-v3', 'x86-64-v2') { Fetch "pc/bin/mr2-$a.exe" "mr2-$a.exe" $bins."mr2-$a"; Fetch "pc/bin/cbet-$a.exe" "cbet-$a.exe" $bins."cbet-$a" }
  Fetch 'src/eq.bin' 'eq.bin' $null; Fetch 'src/p3.bin' 'p3.bin' $null
  Fetch 'src/spin-eqr.txt' 'eqr.txt' $script:job.eqr
  if ($script:job.cb) { Fetch 'src/spin-cb-ranges.json' 'spin-cb-ranges.json' $script:job.cb.ranges }
  if (-not $script:exeM) {
    $t = Start-Process -FilePath (Join-Path $D 'mr2-x86-64-v3.exe') -ArgumentList 'hu', '10', '5', '0' -WorkingDirectory $D -NoNewWindow -PassThru -Wait `
      -RedirectStandardOutput (Join-Path $TMP 'probe.json') -RedirectStandardError (Join-Path $TMP 'probe.txt')
    $a = if ($t.ExitCode -eq 0) { 'x86-64-v3' } else { 'x86-64-v2' }
    $script:exeM = Join-Path $D "mr2-$a.exe"; $script:exeC = Join-Path $D "cbet-$a.exe"; Log "Programme bereit ($a)."
  }
}

# ---- one task = one process ----
$ENVKEYS = 'STACKS', 'MODEL', 'PATCH', 'PF_ISO', 'FULLTREE', 'IPSIZES', 'OOPSIZES', 'TRSIZES', 'PF_HEAP_MB', 'OMP_NUM_THREADS'
$seq = 0
# Each solve runs as cmd /c "program args > out 2> err" with its settings put straight into the start info: Start-Process in
# Windows PowerShell does not hand environment variables set in the script to the program.
function Launch($exe, [string[]]$argv, $envs, $o, $e) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $env:ComSpec
  $q = ($argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
  $psi.Arguments = '/d /s /c ""' + $exe + '" ' + $q + ' > "' + $o + '" 2> "' + $e + '""'
  $psi.WorkingDirectory = $D; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
  foreach ($k in $ENVKEYS) { if ($psi.EnvironmentVariables.ContainsKey($k)) { $psi.EnvironmentVariables.Remove($k) } }
  foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = [string]$envs[$k] }
  [System.Diagnostics.Process]::Start($psi)
}
function Starte($t) {
  $script:seq++; $o = Join-Path $TMP "o$($script:seq).json"; $e = Join-Path $TMP "e$($script:seq).txt"
  if ($t.kind -eq 'pre') {
    $envs = @{ STACKS = $t.k.Replace('|', ','); MODEL = [string]$t.mi; PATCH = '1'; OMP_NUM_THREADS = '1' }
    $p = Launch $exeM @('3h', $t.k.Split('|')[0], [string]$job.iters, '0') $envs $o $e
  } else {
    $fl = Join-Path $TMP "fl$($script:seq).txt"; Set-Content -Path $fl -Value "$($t.fl) 1" -Encoding ASCII
    $envs = @{ PF_HEAP_MB = [string]$t.mb }; foreach ($pr in $t.env.PSObject.Properties) { $envs[$pr.Name] = [string]$pr.Value }
    $p = Launch $exeC @('spin-cb-ranges.json', "$($t.sid)_oop", "$($t.sid)_ip", [string]$t.pot, [string]$t.stack, $fl, '1', '0', '1') $envs $o $e
  }
  if ($jobOk) { try { [void][SpinJob]::Add($p.Handle) } catch {} }
  try { $p.PriorityClass = 'BelowNormal' } catch {}
  [void]$running.Add(@{ p = $p; t = $t; o = $o; e = $e; t0 = Get-Date })
}
function Fertig($r) {
  $t = $r.t; $ok = $r.p.ExitCode -eq 0 -and (Test-Path $r.o) -and (Get-Item $r.o).Length -gt 0
  if (-not $ok) { Log "Fehler bei $($t.name) (Code $($r.p.ExitCode)): $((Get-Content $r.e -Tail 2 -ErrorAction SilentlyContinue) -join ' ')"; $script:fails[$t.key] = 1 + [int]$script:fails[$t.key] }
  elseif ($t.kind -eq 'pre') {
    $js = [IO.File]::ReadAllText($r.o) | ConvertFrom-Json; $ex = ''; foreach ($l in @(Get-Content $r.e)) { if ([string]$l -match 'exploitability') { $ex = ([string]$l).Trim() } }
    $lines = (J ([ordered]@{ v = $job.v; m = $t.m; k = $t.k; ex = $ex; d = (Pack $js.nodes $keep $false) })) + "`n" + (J ([ordered]@{ v = $job.vp; m = $t.m; k = $t.k; pd = (Pack $js.nodes $keep $true) }))
    [IO.File]::AppendAllText($OUT, $lines + "`n", $UTF8)
    [void]$done.Add("$($job.v)|$($t.m)|$($t.k)"); [void]$done.Add("$($job.vp)|$($t.m)|$($t.k)")
  } else {
    $txt = [IO.File]::ReadAllText($r.o).Trim()
    try { $null = $txt | ConvertFrom-Json } catch { Log "Ungültiges Ergebnis bei $($t.name): $($txt.Substring(0, [Math]::Min(160, $txt.Length)))"; $script:fails[$t.key] = 1 + [int]$script:fails[$t.key]; Remove-Item $r.o, $r.e -ErrorAction SilentlyContinue; return }
    [IO.File]::AppendAllText($OUT, '{"v":"' + $job.cb.v + '","sid":"' + $t.sid + '","r":' + $txt + "}`n", $UTF8)
    [void]$done.Add("$($job.cb.v)|$($t.sid)|$($t.fl)")
  }
  if ($ok -and $Max -gt 0 -and $script:nDone + 1 -ge $Max) { $script:nDone++; AllesStoppen; Log "Test fertig: $($script:nDone) Ergebnisse."; exit 0 }
  if ($ok) { $script:nDone++; $min = [Math]::Round(((Get-Date) - $r.t0).TotalMinutes, 1); Log "fertig: $($t.name) in $min min ($($script:nDone) seit Start, noch $($script:todo.Count) offen)" }
  Remove-Item $r.o, $r.e -ErrorAction SilentlyContinue
}

$MODELS = 'cev', 'x10', 'x50', 'jp'
$fails = @{}; $nDone = 0; $todo = New-Object System.Collections.ArrayList; $keep = $null; $nextJob = Get-Date
Log 'Start. Fenster offen lassen; schließen beendet die Rechnungen (beim nächsten Start geht es weiter).'
while ($true) {
  if (GGOffen) { AllesStoppen; Log 'GGPoker wurde geöffnet: alle Rechnungen beendet, Rechner aus.'; Read-Host 'Enter zum Beenden'; exit 1 }
  if ((Get-Date) -ge $nextJob -and $running.Count -eq 0) {   # read the job again between batches
    try { Vorbereiten } catch { Log $_.Exception.Message; Start-Sleep 300; continue }
    $keep = New-Object 'System.Collections.Generic.HashSet[string]'; foreach ($s in $job.spots) { [void]$keep.Add([string]$s) }
    $todo.Clear()
    function AddPre($lst, $vers) { for ($i = 0; $i -lt @($lst).Count -and -not $NurFlops; $i++) { $m = $lst[$i][0]; $k = $lst[$i][1]; $ver = $vers[$i]
      if (-not $done.Contains("$ver|$m|$k") -and [int]$fails["pre|$m|$k"] -lt 2) { [void]$todo.Add(@{ kind = 'pre'; m = $m; k = $k; mi = $MODELS.IndexOf($m); mb = 300; name = "Preflop $m $k"; key = "pre|$m|$k" }) } } }
    AddPre $job.pre $job.prever   # ChipEV preflop, then the flops, then the payout models (x10, jackpot, x50) last
    if ($job.cb) { foreach ($c in $job.cb.tasks) { if (-not $done.Contains("$($job.cb.v)|$($c[0])|$($c[1])") -and [int]$fails["cb|$($c[0])|$($c[1])"] -lt 2) {
      [void]$todo.Add(@{ kind = 'cb'; sid = $c[0]; fl = $c[1]; pot = $c[2]; stack = $c[3]; env = $c[4]; mb = [int]$c[5]; name = "Flop $($c[0]) $($c[1])"; key = "cb|$($c[0])|$($c[1])" }) } } }
    if ($job.pre_late) { AddPre $job.pre_late $job.prever_late }
    Log "Auftrag: $($todo.Count) Aufgaben offen."
    if ($todo.Count -eq 0) { Log 'Alles fertig. Ergebnis mit Ergebnis-einpacken.bat einpacken und an Claude schicken.'; Start-Sleep 1800; $nextJob = Get-Date; continue }
    $nextJob = (Get-Date).AddHours(2)
  }
  foreach ($r in @($running)) { if ($r.p.HasExited) { $running.Remove($r); Fertig $r } }
  $used = 0; foreach ($r in $running) { $used += $r.t.mb }
  while ($running.Count -lt $Kerne -and $todo.Count -gt 0 -and (Get-Date) -lt $nextJob) {   # next task that fits into the memory budget (every 2 h: let them finish, then read the job again)
    $pick = $null; foreach ($t in $todo) { if ($used + $t.mb -le $budget -and $t.mb + 500 -le (FreiMB)) { $pick = $t; break } }
    if (-not $pick) { break }
    $todo.Remove($pick); Starte $pick; $used += $pick.mb
  }
  if ($running.Count -eq 0 -and $todo.Count -eq 0) { $nextJob = Get-Date }
  Start-Sleep -Seconds 3
}
