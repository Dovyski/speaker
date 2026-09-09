# i47 P0 spike: discover the Windows Terminal window title hosting this hook's
# Claude Code session, and match it to an hwnd from speak.exe --list-targets.
#
# Usable as a Claude Code `Stop` hook. Reads hook JSON from stdin, writes one
# JSON line per invocation to ~/.claude/attention/p0-spike.log, never writes to
# stdout, always exits 0.

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$LogDir   = Join-Path $env:USERPROFILE '.claude\attention'
$LogFile  = Join-Path $LogDir 'p0-spike.log'
$SpeakExe = 'C:\Dev\www\claude-speak\speak.exe'

$rec = [ordered]@{
    ts           = (Get-Date).ToString('o')
    pid          = $PID
    psedition    = $PSVersionTable.PSEdition
    psversion    = $PSVersionTable.PSVersion.ToString()
    in_redirect  = $null
    out_redirect = $null
    hook         = $null
    chain        = @()
    methods      = @()
    title        = $null
    title_source = $null
    hwnd         = $null
    hwnd_title   = $null
    candidates   = @()
    ms           = $null
    errors       = @()
}
$sw0 = [System.Diagnostics.Stopwatch]::StartNew()

function Add-Err([string]$where, $e) {
    try { $rec.errors += ("{0}: {1}" -f $where, (($e | Out-String).Trim() -replace '\s+', ' ')) } catch {}
}
function Add-Method([string]$name, $value, $extra) {
    $m = [ordered]@{ method = $name; value = $value }
    if ($extra) { foreach ($k in $extra.Keys) { $m[$k] = $extra[$k] } }
    $rec.methods += $m
}

# --- 0. stdin (hook payload) -------------------------------------------------
try {
    $rec.in_redirect  = [Console]::IsInputRedirected
    $rec.out_redirect = [Console]::IsOutputRedirected
    if ([Console]::IsInputRedirected) {
        $raw = [Console]::In.ReadToEnd()
        if ($raw) {
            $j = $raw | ConvertFrom-Json
            $rec.hook = [ordered]@{
                session_id      = $j.session_id
                cwd             = $j.cwd
                hook_event_name = $j.hook_event_name
                transcript_path = $j.transcript_path
            }
        }
    }
} catch { Add-Err 'stdin' $_ }

# --- 1. P/Invoke surface -----------------------------------------------------
$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class I47 {
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AllocConsole();
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
        public static extern uint GetConsoleTitleW(StringBuilder sb, uint size);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
        public static extern uint GetConsoleOriginalTitleW(StringBuilder sb, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
        public static extern uint GetConsoleProcessList(uint[] list, uint count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
        public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);

    public static string WinText(IntPtr h) {
        if (h == IntPtr.Zero) return null;
        var sb = new StringBuilder(1024);
        int n = GetWindowTextW(h, sb, sb.Capacity);
        return n > 0 ? sb.ToString() : null;
    }
    static string Clean(StringBuilder sb, uint n) {
        if (n == 0) return null;
        string s = sb.ToString();
        if (n < (uint)s.Length) s = s.Substring(0, (int)n);   // buffer tail is uninitialised
        int z = s.IndexOf('\0');
        if (z >= 0) s = s.Substring(0, z);
        // ConPTY leaves control bytes (e.g. BEL from OSC 0) in the title
        var o = new StringBuilder(s.Length);
        foreach (char c in s) { if (!char.IsControl(c)) o.Append(c); }
        s = o.ToString().Trim();
        return s.Length == 0 ? null : s;
    }
    public static string ConTitle() {
        var sb = new StringBuilder(1024);
        return Clean(sb, GetConsoleTitleW(sb, (uint)sb.Capacity));
    }
    public static string ConOrigTitle() {
        var sb = new StringBuilder(1024);
        return Clean(sb, GetConsoleOriginalTitleW(sb, (uint)sb.Capacity));
    }
    public static uint[] ConProcs() {
        uint[] buf = new uint[64];
        uint n = GetConsoleProcessList(buf, (uint)buf.Length);
        if (n == 0 || n > buf.Length) return new uint[0];
        uint[] r = new uint[n];
        Array.Copy(buf, r, (int)n);
        return r;
    }
    public static int Err() { return Marshal.GetLastWin32Error(); }
}
'@
$havePinvoke = $false
try {
    if (-not ('I47' -as [type])) { Add-Type -TypeDefinition $sig -Language CSharp | Out-Null }
    $havePinvoke = $true
} catch { Add-Err 'add-type' $_ }

# --- 2. parent process chain -------------------------------------------------
$chainPids = @()
try {
    $stopAt = @('windowsterminal.exe', 'openconsole.exe', 'conhost.exe', 'explorer.exe')
    $cur = $PID
    for ($i = 0; $i -lt 12; $i++) {
        $p = $null
        try { $p = Get-CimInstance Win32_Process -Filter "ProcessId=$cur" -ErrorAction Stop } catch {}
        if (-not $p) { break }
        $rec.chain += [ordered]@{ pid = [int]$p.ProcessId; name = $p.Name; ppid = [int]$p.ParentProcessId }
        $chainPids += [int]$p.ProcessId
        if ($stopAt -contains $p.Name.ToLower()) { break }
        if (-not $p.ParentProcessId -or $p.ParentProcessId -eq 0 -or $p.ParentProcessId -eq $cur) { break }
        $cur = [int]$p.ParentProcessId
    }
} catch { Add-Err 'chain' $_ }

# --- 3a. non-destructive probes ---------------------------------------------
try { Add-Method 'Console::Title' ([Console]::Title) $null } catch { Add-Method 'Console::Title' $null @{ error = $_.Exception.Message } }
try { Add-Method 'RawUI.WindowTitle' ($Host.UI.RawUI.WindowTitle) $null } catch { Add-Method 'RawUI.WindowTitle' $null @{ error = $_.Exception.Message } }

if ($havePinvoke) {
    try {
        $cw = [I47]::GetConsoleWindow()
        $x = @{ console_hwnd = [int64]$cw }
        if ($cw -ne [IntPtr]::Zero) {
            $opid = 0
            [void][I47]::GetWindowThreadProcessId($cw, [ref]$opid)
            $x.owner_pid = [int]$opid
            try { $x.owner_proc = (Get-Process -Id $opid -ErrorAction Stop).ProcessName } catch {}
        }
        Add-Method 'GetConsoleWindow+GetWindowTextW' ([I47]::WinText($cw)) $x
    } catch { Add-Err 'getconsolewindow' $_ }

    try { Add-Method 'GetConsoleTitleW(inherited)' ([I47]::ConTitle()) $null } catch { Add-Err 'contitle' $_ }
    try { Add-Method 'GetConsoleOriginalTitleW(inherited)' ([I47]::ConOrigTitle()) @{ diag = $true } } catch { Add-Err 'conorigtitle' $_ }
    try {
        $procs = [I47]::ConProcs()
        $names = @()
        foreach ($q in $procs) { try { $names += ("{0}:{1}" -f $q, (Get-Process -Id $q -ErrorAction Stop).ProcessName) } catch { $names += ("{0}:?" -f $q) } }
        Add-Method 'GetConsoleProcessList(inherited)' ($names -join ',') @{ diag = $true; count = $procs.Count }
    } catch { Add-Err 'conproclist' $_ }

    # --- 3b. FreeConsole + AttachConsole(ancestor) + GetConsoleTitleW -------
    # Destructive: runs last. Each attempt frees first, because AttachConsole
    # returns ERROR_ACCESS_DENIED (5) when the caller already owns a console.
    foreach ($apid in $chainPids) {
        if ($apid -eq $PID) { continue }
        $x = [ordered]@{ target_pid = $apid }
        try { $x.target_proc = (Get-Process -Id $apid -ErrorAction Stop).ProcessName } catch { $x.target_proc = '?' }
        $val = $null
        try {
            [void][I47]::FreeConsole()
            $ok = [I47]::AttachConsole([uint32]$apid)
            $x.attached = $ok
            if (-not $ok) { $x.gle = [I47]::Err() }
            else {
                $val = [I47]::ConTitle()
                $cw2 = [I47]::GetConsoleWindow()
                $x.console_hwnd = [int64]$cw2
                $x.window_text  = [I47]::WinText($cw2)
                $x.proc_list    = ([I47]::ConProcs() -join ',')
            }
        } catch { $x.error = $_.Exception.Message }
        Add-Method 'AttachConsole+GetConsoleTitleW' $val $x
    }
    try { [void][I47]::FreeConsole() } catch {}
}

# --- 4. pick the best title --------------------------------------------------
function Normalize([string]$s) {
    if (-not $s) { return $null }
    # strip a leading spinner/status glyph run (non letter/digit/open bracket)
    $t = [regex]::Replace($s, '^[^\p{L}\p{N}\(\[]+', '')
    return $t.Trim()
}
$badExact = @('', 'Windows PowerShell', 'Administrator: Windows PowerShell', 'Command Prompt')
try {
    foreach ($m in $rec.methods) {
        if ($m.Contains('diag')) { continue }              # process lists, original titles
        $v = $m.value
        if (-not $v) { continue }
        if ($badExact -contains $v) { continue }
        if ($v -match '(?i)\.(exe|cmd|bat|ps1)$') { continue }   # "…\pwsh.exe", "…\run.cmd"
        if ($v -match '(?i)^(pwsh|powershell|cmd|node|bash)$') { continue }
        $rec.title = $v
        $rec.title_source = $m.method
        break
    }
} catch { Add-Err 'pick-title' $_ }

# --- 5. match against speak.exe --list-targets -------------------------------
# Launched through System.Diagnostics.Process with redirected stdout: a plain
# `& speak.exe` fails with 0x6 "handle is invalid" once FreeConsole ran.
try {
    if (Test-Path $SpeakExe) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $SpeakExe
        $psi.Arguments = '--list-targets'
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
        $proc = [System.Diagnostics.Process]::Start($psi)
        $json = $proc.StandardOutput.ReadToEnd()
        [void]$proc.WaitForExit(3000)

        $targets = $json | ConvertFrom-Json
        $wt = @($targets | Where-Object { $_.process -match '(?i)windowsterminal' })
        $rec.candidates = @($wt | ForEach-Object { [ordered]@{ hwnd = $_.hwnd; title = $_.title } })
        $needle = Normalize $rec.title
        if ($needle) {
            $hit = $null
            foreach ($t in $wt) { if ((Normalize $t.title) -eq $needle) { $hit = $t; break } }
            if (-not $hit) {
                foreach ($t in $wt) {
                    $tn = Normalize $t.title
                    if ($tn -and ($tn.Contains($needle) -or $needle.Contains($tn))) { $hit = $t; break }
                }
            }
            if ($hit) { $rec.hwnd = $hit.hwnd; $rec.hwnd_title = $hit.title }
        }
    } else { Add-Err 'speak' 'speak.exe not found' }
} catch { Add-Err 'list-targets' $_ }

# --- 6. append log line ------------------------------------------------------
try {
    $rec.ms = [int]$sw0.ElapsedMilliseconds
    if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Force -Path $LogDir | Out-Null }
    $line = $rec | ConvertTo-Json -Depth 8 -Compress
    $s = New-Object System.IO.StreamWriter($LogFile, $true, (New-Object System.Text.UTF8Encoding($false)))
    try { $s.WriteLine($line) } finally { $s.Dispose() }
} catch {}

exit 0
