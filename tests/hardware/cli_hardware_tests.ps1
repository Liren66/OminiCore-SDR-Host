<#
.SYNOPSIS
    End-to-end CLI tests against a connected OmniCoreSDR board.

.DESCRIPTION
    Runs och.exe as a user would and checks exit codes, JSON results, files on
    disk and throughput. Groups:

      basic      version, discovery, info, status, catalog, clock, FIR, temperature
      control    get/set round trips (values are restored afterwards)
      args       argument validation and exit codes (no device access)
      capture    RX file capture: sizes, formats, metadata sidecar, overwrite guard
      playback   TX file playback (once / loop), generated tone, short-file guard
      bench      throughput at several rates, channel counts and IQ widths
      output     human-readable output: plain ASCII when redirected, 78 columns

    TX tests transmit RF. They use --attenuation (default 60 dB) to keep the
    output level low; connect a load or an antenna as appropriate.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\hardware\cli_hardware_tests.ps1
.EXAMPLE
    powershell -File tests\hardware\cli_hardware_tests.ps1 -Group capture,bench -BenchSeconds 10
.EXAMPLE
    powershell -File tests\hardware\cli_hardware_tests.ps1 -SkipTx -Och "C:\Program Files\OmniCore Host\bin\och.exe"
#>
param(
    [string]$Och = '',
    [string]$Serial = '',
    [string]$OutDir = '',
    [string[]]$Group = @('basic', 'control', 'args', 'capture', 'playback', 'bench', 'output'),
    [ValidateRange(1, 600)][int]$BenchSeconds = 5,
    [ValidateRange(0, 89)][double]$TxAttenuation = 60,
    [ValidateRange(0.5, 1.0)][double]$MinThroughputRatio = 0.95,
    [switch]$SkipTx,
    [switch]$KeepFiles
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$Och) { $Och = Join-Path $root 'build\och.exe' }
if (!(Test-Path -LiteralPath $Och)) { throw "och.exe not found: $Och (build first or pass -Och)" }
$Och = (Resolve-Path -LiteralPath $Och).Path
if (!$OutDir) { $OutDir = Join-Path ([IO.Path]::GetTempPath()) ('och-cli-tests-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$Group = @($Group | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
$script:results = New-Object System.Collections.Generic.List[object]
$script:benchRows = New-Object System.Collections.Generic.List[object]
$script:detail = ''
$script:warning = ''
$script:skip = ''

function Quote-Arg([string]$a) {
    if ($a -eq '') { return '""' }
    if ($a -notmatch '[\s"]') { return $a }
    return '"' + ($a -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}
function Invoke-Och {
    param([string[]]$Arguments, [int]$TimeoutSec = 180, [switch]$NoSerial)
    $list = @($Arguments)
    if ($Serial -and -not $NoSerial) { $list += @('--serial', $Serial) }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Och
    $psi.Arguments = ($list | ForEach-Object { Quote-Arg $_ }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.WorkingDirectory = $OutDir
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch {}
        throw "och $($psi.Arguments) timed out after $TimeoutSec s"
    }
    $p.WaitForExit()
    [pscustomobject]@{
        ExitCode = $p.ExitCode; Stdout = $stdout.Result; Stderr = $stderr.Result
        Seconds = $watch.Elapsed.TotalSeconds; Command = "och $($psi.Arguments)"
    }
}
function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-Exit($Run, [int]$Expected) {
    if ($Run.ExitCode -ne $Expected) {
        $tail = ($Run.Stderr + $Run.Stdout).Trim()
        if ($tail.Length -gt 400) { $tail = $tail.Substring($tail.Length - 400) }
        throw "$($Run.Command) exited $($Run.ExitCode), expected $Expected. $tail"
    }
}
function From-Json([string]$Text) {
    $line = ($Text -split "`r?`n" | Where-Object { $_.Trim().StartsWith('{') -or $_.Trim().StartsWith('[') } | Select-Object -Last 1)
    Assert ([bool]$line) 'no JSON on stdout'
    return $line | ConvertFrom-Json
}
function Json-Lines([string]$Text) {
    $Text -split "`r?`n" | Where-Object { $_.Trim().StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json }
}
function Set-Detail([string]$Text) { $script:detail = $Text }
function Set-Warn([string]$Text) { $script:warning = $Text }
function Skip-Test([string]$Text) { $script:skip = $Text; throw 'skip' }

function Test-Case([string]$GroupName, [string]$Name, [scriptblock]$Body) {
    if ($Group -notcontains $GroupName) { return }
    $script:detail = ''; $script:warning = ''; $script:skip = ''
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $status = 'PASS'
    try { & $Body | Out-Null }
    catch {
        if ($script:skip) { $status = 'SKIP'; $script:detail = $script:skip }
        else { $status = 'FAIL'; $script:detail = $_.Exception.Message }
    }
    if ($status -eq 'PASS' -and $script:warning) { $status = 'WARN'; $script:detail = ($script:warning + ' ' + $script:detail).Trim() }
    $script:results.Add([pscustomobject]@{
        Group = $GroupName; Name = $Name; Status = $status
        Seconds = [math]::Round($watch.Elapsed.TotalSeconds, 2); Detail = $script:detail })
    $colour = @{ PASS = 'Green'; WARN = 'Yellow'; FAIL = 'Red'; SKIP = 'DarkGray' }[$status]
    Write-Host ('  {0,-5} ' -f $status) -ForegroundColor $colour -NoNewline
    Write-Host ('{0,-9} {1}' -f $GroupName, $Name) -NoNewline
    if ($script:detail) { Write-Host ('  - ' + $script:detail) -ForegroundColor DarkGray } else { Write-Host '' }
}
function Read-Head([string]$Path, [int]$Count) {
    $fs = [IO.File]::OpenRead($Path)
    try { $buffer = New-Object byte[] $Count; $n = $fs.Read($buffer, 0, $Count); if ($n -lt $Count) { $buffer = $buffer[0..($n - 1)] }; return ,$buffer }
    finally { $fs.Dispose() }
}
function New-Path([string]$Name) { Join-Path $OutDir $Name }

# Collects one benchmark result and judges it.
function Measure-Bench([string]$Label, [string[]]$Arguments, [double]$Rate, [int]$RxChannels, [int]$TxChannels, [int]$WireBytes) {
    $run = Invoke-Och (@('bench') + $Arguments + @('--seconds', "$BenchSeconds", '--json')) -TimeoutSec ($BenchSeconds + 60)
    Assert-Exit $run 0
    $r = From-Json $run.Stdout
    $notes = @(); $hard = @(); $soft = @()
    foreach ($dir in @('rx', 'tx')) {
        $s = $r.$dir
        if ($null -eq $s) { continue }
        $channels = if ($dir -eq 'rx') { $RxChannels } else { $TxChannels }
        $expected = $Rate * $channels * $WireBytes / 1e6
        $ratio = if ($expected -gt 0) { $s.usb_MBps / $expected } else { 0 }
        $script:benchRows.Add([pscustomobject]@{
            Test = $Label; Dir = $dir.ToUpper(); Channels = $channels; MSPS = [math]::Round($Rate / 1e6, 2)
            Seconds = [math]::Round($s.elapsed_seconds, 2); MBps = [math]::Round($s.usb_MBps, 1)
            Expected = [math]::Round($expected, 1); Ratio = [math]::Round($ratio * 100, 1)
            Overflows = $s.device_overflows; Underflows = $s.device_underflows; Resyncs = $s.resyncs
            HostDrops = $s.host_dropped_samples; TransferErrors = $s.transfer_errors })
        $notes += ('{0} {1:N1} MB/s ({2:N1}%)' -f $dir.ToUpper(), $s.usb_MBps, ($ratio * 100))
        if ($s.transfer_errors -gt 0 -or $s.host_dropped_samples -gt 0 -or $s.event_drops -gt 0) {
            $hard += "$dir transfer_errors=$($s.transfer_errors) host_dropped=$($s.host_dropped_samples) event_drops=$($s.event_drops)"
        }
        if ($ratio -lt ($MinThroughputRatio - 0.05)) { $hard += ('{0} throughput {1:N1}% of wire rate' -f $dir, ($ratio * 100)) }
        elseif ($ratio -lt $MinThroughputRatio) { $soft += ('{0} throughput {1:N1}% of wire rate' -f $dir, ($ratio * 100)) }
        if ($s.device_overflows -gt 0 -or $s.device_underflows -gt 0 -or $s.tx_starvation_events -gt 0) {
            $soft += "$dir overflow=$($s.device_overflows) underflow=$($s.device_underflows) starvation=$($s.tx_starvation_events)"
        }
    }
    Set-Detail ($notes -join ', ')
    if ($hard.Count) { throw (($hard + $notes) -join '; ') }
    if ($soft.Count) { Set-Warn (($soft | Select-Object -Unique) -join '; ') }
}

# Test conditions: host, USB path and device, recorded with every run.
function Get-TestEnvironment {
    $info = [ordered]@{}
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
        $info.os = '{0} {1} (build {2}.{3}, {4})' -f $os.Caption.Trim(), $cv.DisplayVersion, $os.BuildNumber, $cv.UBR, $os.OSArchitecture
    } catch {}
    try {
        $cs = Get-CimInstance Win32_ComputerSystem
        $info.computer = if ($cs.SystemFamily) { '{0} {1} ({2})' -f $cs.Manufacturer, $cs.SystemFamily, $cs.Model } else { '{0} {1}' -f $cs.Manufacturer, $cs.Model }
        $modules = @(Get-CimInstance Win32_PhysicalMemory)
        $total = ($modules | Measure-Object Capacity -Sum).Sum
        $type = @{ 24 = 'DDR3'; 26 = 'DDR4'; 34 = 'DDR5'; 35 = 'LPDDR5' }[[int]$modules[0].SMBIOSMemoryType]
        $info.memory = '{0:N0} GiB ({1} x {2:N0} GiB {3}-{4})' -f ($total / 1GB), $modules.Count, ($modules[0].Capacity / 1GB), $type, $modules[0].ConfiguredClockSpeed
    } catch {}
    try {
        $battery = @(Get-CimInstance Win32_Battery -ErrorAction Stop)
        $info.power_source = if (!$battery.Count) { 'AC (no battery)' } elseif ($battery[0].BatteryStatus -eq 2) { 'AC' } else { 'battery' }
    } catch {}
    try {
        $cpu = @(Get-CimInstance Win32_Processor)[0]
        $info.cpu = '{0} ({1} cores / {2} threads)' -f $cpu.Name.Trim(), $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors
    } catch {}
    try { $plan = (powercfg /getactivescheme) -join ' '; if ($plan -match '\((.+)\)') { $info.power_plan = $Matches[1] } } catch {}
    $info.powershell = $PSVersionTable.PSVersion.ToString()
    try {
        $dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_1A86&PID_5537' } | Select-Object -First 1
        if ($dev) {
            $drv = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -eq $dev.InstanceId } | Select-Object -First 1
            if ($drv) { $info.usb_driver = ('{0} {1}' -f $drv.DriverProviderName, $drv.DriverVersion).Trim() }
            $id = $dev.InstanceId
            for ($i = 0; $i -lt 8 -and $id; $i++) {
                $parent = (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_Parent' -ErrorAction Stop).Data
                if (!$parent) { break }
                $p = Get-PnpDevice -InstanceId $parent -ErrorAction Stop
                if ($p.FriendlyName -match 'Host Controller|xHCI|eXtensible|主机控制器') { $info.usb_controller = $p.FriendlyName; break }
                $id = $parent
            }
        }
    } catch {}
    try {
        $letter = (Split-Path -Qualifier $OutDir).TrimEnd(':')
        $disk = Get-Partition -DriveLetter $letter | Get-Disk
        $pd = Get-PhysicalDisk | Where-Object { $_.DeviceId -eq [string]$disk.Number } | Select-Object -First 1
        $info.capture_disk = ('{0} ({1}, {2})' -f $disk.FriendlyName.Trim(), $pd.MediaType, $disk.BusType)
    } catch {}
    return [pscustomobject]$info
}

# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '  OmniCore Host - CLI hardware tests' -ForegroundColor Cyan
Write-Host "  och.exe : $Och"
Write-Host "  output  : $OutDir"
Write-Host "  groups  : $($Group -join ', ')   bench: $BenchSeconds s   TX attenuation: $TxAttenuation dB$(if ($SkipTx) { '   (TX skipped)' })"
Write-Host ''

$version = (Invoke-Och @('version', '--json') -NoSerial)
$script:expectedVersion = (From-Json $version.Stdout).version

# Discover the device first; every device command uses --serial afterwards.
$found = Invoke-Och @('find', '--json') -NoSerial
$devices = @()
if ($found.ExitCode -eq 0) { $devices = @(From-Json $found.Stdout | ForEach-Object { $_ }) }
if (!$Serial -and $devices.Count -ge 1) { $Serial = $devices[0].serial }
$deviceGroups = @('basic', 'control', 'capture', 'playback', 'bench', 'output')
if (!$Serial) {
    Write-Host '  No device found; only device-independent tests will run.' -ForegroundColor Yellow
    $Group = @($Group | Where-Object { $deviceGroups -notcontains $_ -or $_ -eq 'basic' })
}
$tx = @('--attenuation', "$TxAttenuation")

$environment = Get-TestEnvironment
if ($Serial) {
    try {
        $i = From-Json (Invoke-Och @('info', '--json')).Stdout
        $environment | Add-Member device ('{0} {1}, firmware {2}, USB link {3} Mbit/s' -f $i.name, $i.serial, $i.firmware, $i.usb_speed_mbps)
    } catch {}
}
$environment | Add-Member och ('{0} ({1})' -f $script:expectedVersion, $Och)
Write-Host '  Test conditions' -ForegroundColor Cyan
foreach ($prop in $environment.PSObject.Properties) {
    if ($prop.Name -ne 'och') { Write-Host ('  {0,-15} {1}' -f $prop.Name, $prop.Value) }
}
Write-Host ''

# ---------------------------------------------------------------------------
# basic
# ---------------------------------------------------------------------------
Test-Case basic 'version reports version and ABI 1' {
    $v = From-Json $version.Stdout
    Assert ($v.abi -eq 1) "abi=$($v.abi)"
    Assert ($v.version -match '^\d+\.\d+\.\d+$') "version=$($v.version)"
    Set-Detail "version $($v.version)"
}
Test-Case basic 'find lists the board' {
    Assert-Exit $found 0
    Assert ($devices.Count -ge 1) 'no device enumerated'
    $d = $devices | Where-Object { $_.serial -eq $Serial } | Select-Object -First 1
    Assert ($null -ne $d) "serial $Serial not enumerated"
    Assert ($d.rx_channels -eq 2 -and $d.tx_channels -eq 2) "channels $($d.rx_channels)x$($d.tx_channels)"
    Set-Detail "$($devices.Count) device(s), using $Serial"
}
Test-Case basic 'info reports SuperSpeed link and firmware' {
    $run = Invoke-Och @('info', '--json'); Assert-Exit $run 0
    $i = From-Json $run.Stdout
    Assert ($i.serial -eq $Serial) 'serial mismatch'
    Assert ($i.firmware.Length -gt 0) 'empty firmware string'
    Assert ($i.max_sample_rate -eq 61440000) "max_sample_rate=$($i.max_sample_rate)"
    Assert ($null -ne $i.power) 'power object missing'
    if ($i.usb_speed_mbps -lt 5000) { Set-Warn "USB link $($i.usb_speed_mbps) Mbit/s (expected >= 5000)" }
    Set-Detail "$($i.firmware), USB $($i.usb_speed_mbps) Mbit/s"
}
Test-Case basic 'system.status reports initialised RF' {
    $run = Invoke-Och @('get', 'system.status', '--json'); Assert-Exit $run 0
    $r = From-Json $run.Stdout
    Assert ($r.reply -match 'init=ready') 'init is not ready'
    Assert ($r.reply -match 'digital=ready') 'digital interface is not ready'
}
Test-Case basic 'commands catalog is valid JSON' {
    $run = Invoke-Och @('commands', '--json') -NoSerial; Assert-Exit $run 0
    $text = $run.Stdout
    Assert ($text -match '"rx\.lo-frequency"') 'rx.lo-frequency missing'
    $null = $text | ConvertFrom-Json
}
Test-Case basic 'clock status has matching RX/TX sample rates' {
    $run = Invoke-Och @('clock', 'status', '--json'); Assert-Exit $run 0
    $c = From-Json $run.Stdout
    Assert ($c.reference_hz -eq 40000000) "reference_hz=$($c.reference_hz)"
    Assert ($c.rx_hz[5] -eq $c.tx_hz[5]) 'RX/TX sample rates differ'
    Set-Detail ('Fs {0:N3} MSPS' -f ($c.rx_hz[5] / 1e6))
}
Test-Case basic 'FIR status is a verified snapshot' {
    $run = Invoke-Och @('fir', 'status', '--json'); Assert-Exit $run 0
    $f = From-Json $run.Stdout
    foreach ($d in @($f.rx, $f.tx)) {
        Assert ($d.tap_count % 16 -eq 0 -and $d.tap_count -le 128) "tap_count=$($d.tap_count)"
        Assert ($d.coefficients.Count -eq $d.tap_count) 'coefficient count mismatch'
    }
}
Test-Case basic 'Zynq core temperature is valid and plausible' {
    $run = Invoke-Och @('get', 'core.temperature', '--json'); Assert-Exit $run 0
    $r = From-Json $run.Stdout
    Assert ($r.reply -match 'valid=1') 'temperature not valid'
    $c = [double]([regex]::Match($r.reply, 'Celsius=([\d.]+)').Groups[1].Value)
    Assert ($c -gt 0 -and $c -lt 105) "temperature $c C"
    if ($c -gt 85) { Set-Warn "core temperature $c C" }
    Set-Detail "$c C"
}
Test-Case basic 'digital loopback self-test' {
    $run = Invoke-Och @('selftest', '--json') -TimeoutSec 60; Assert-Exit $run 0
    $r = From-Json $run.Stdout
    Assert ($r.reply -notmatch '(?i)fail|error=[1-9]') "self-test reply: $($r.reply)"
}

# ---------------------------------------------------------------------------
# control
# ---------------------------------------------------------------------------
function Get-Scalar([string]$Name) {
    $run = Invoke-Och @('get', $Name, '--json'); Assert-Exit $run 0
    $r = From-Json $run.Stdout
    return [regex]::Match($r.reply, [regex]::Escape($Name) + '=([-\d.]+)').Groups[1].Value
}
Test-Case control 'rx.lo-frequency set/get round trip' {
    $old = Get-Scalar 'rx.lo-frequency'
    try {
        Assert-Exit (Invoke-Och @('set', 'rx.lo-frequency', '2.4G', '--json')) 0
        $now = Get-Scalar 'rx.lo-frequency'
        Assert ([double]$now -eq 2.4e9) "read back $now"
    } finally { if ($old) { $null = Invoke-Och @('set', 'rx.lo-frequency', $old) } }
    Set-Detail "restored $old Hz"
}
Test-Case control 'tx1.attenuation set/get round trip' {
    $old = Get-Scalar 'tx1.attenuation'
    try {
        Assert-Exit (Invoke-Och @('set', 'tx1.attenuation', '23.5', '--json')) 0
        $now = Get-Scalar 'tx1.attenuation'
        Assert ([math]::Abs([double]$now - 23.5) -lt 0.01) "read back $now"
    } finally { if ($old) { $null = Invoke-Och @('set', 'tx1.attenuation', $old) } }
    Set-Detail "restored $old dB"
}
Test-Case control 'config applies RF settings without starting TX' {
    $run = Invoke-Och @('config', '--sample-rate', '15.36M', '--lo-frequency', '915M', '--bandwidth', '10M', '--gain', '20', '--json')
    Assert-Exit $run 0
    $c = From-Json $run.Stdout
    Assert ($c.sample_rate -eq 15360000 -and $c.rx_frequency -eq 915000000 -and $c.rx_agc -eq 0) 'readback mismatch'
    try {
        $status = From-Json (Invoke-Och @('get', 'system.status', '--json')).Stdout
        Assert ($status.reply -match 'tx_actual_mask=0') 'TX became active'
    } finally { $null = Invoke-Och @('config', '--sample-rate', '30.72M') }  # back to the host defaults
}

# ---------------------------------------------------------------------------
# args (exit code 2, nothing touches the device)
# ---------------------------------------------------------------------------
$argCases = @(
    @('unknown option', @('bench', '--rx', '--bogus')),
    @('bench needs a direction', @('bench', '--seconds', '1')),
    @('rx needs --file', @('rx', '--seconds', '1')),
    @('bench rejects --file', @('bench', '--rx', '--file', 'x.sc16')),
    @('unknown canonical command', @('get', 'nonsense.key')),
    @('invalid number suffix', @('bench', '--rx', '--sample-rate', '30.72X')),
    @('invalid channel list', @('bench', '--rx', '--channels', '2,1')),
    @('sc8 requires 8-bit wire', @('rx', '--file', 'x.sc8', '--format', 'sc8')),
    @('8-bit wire requires duplex', @('bench', '--rx', '--iq-bits', '8')),
    @('block must be a multiple of 49152', @('bench', '--rx', '--block', '50000')),
    @('conflicting alias and option', @('bench', '--rx', '--rate', '1M', '--sample-rate', '2M')),
    @('--loop requires a file', @('tx', '--loop'))
)
foreach ($case in $argCases) {
    $name = $case[0]; $arguments = $case[1]
    Test-Case args "exit 2: $name" {
        $run = Invoke-Och ($arguments + @('--json')) -NoSerial
        Assert-Exit $run 2
        $err = Json-Lines $run.Stderr | Select-Object -Last 1
        Assert ($null -ne $err -and $err.exit_code -eq 2 -and $err.error) 'missing JSON error on stderr'
        Set-Detail $err.error
    }
}
Test-Case args 'text-mode error is readable' {
    $run = Invoke-Och @('get', 'nonsense.key') -NoSerial
    Assert-Exit $run 2
    Assert ($run.Stderr -match '\[FAIL\]\s+Invalid argument') 'no [FAIL] line on stderr'
    Assert ($run.Stdout.Trim() -eq '') 'unexpected stdout'
}

# ---------------------------------------------------------------------------
# capture
# ---------------------------------------------------------------------------
$captureRate = 15360000
$captureSc16 = New-Path 'capture-2ch.sc16'
function Test-Capture([string]$Path, [string]$Format, [string]$Channels, [double]$Seconds, [int]$BytesPerSample) {
    $run = Invoke-Och @('rx', '--file', $Path, '--format', $Format, '--channels', $Channels, '--sample-rate', "$captureRate",
                       '--seconds', "$Seconds", '--overwrite', '--json') -TimeoutSec 120
    Assert-Exit $run 0
    $r = From-Json $run.Stdout
    $count = if ($Channels -eq '1,2') { 2 } else { 1 }
    $frame = $BytesPerSample * $count
    $size = (Get-Item -LiteralPath $Path).Length
    Assert ($size -gt 0) 'empty capture'
    Assert ($size % $frame -eq 0) "size $size is not a multiple of the $frame-byte frame"
    $samples = $size / $frame
    Assert ($samples -eq $r.rx.samples_per_channel) "file holds $samples samples/ch, stats report $($r.rx.samples_per_channel)"
    $expected = $captureRate * $Seconds
    Assert ($samples -ge 0.9 * $expected) ('only {0:N0} of ~{1:N0} samples/ch' -f $samples, $expected)
    Assert ($r.rx.transfer_errors -eq 0 -and $r.rx.host_dropped_samples -eq 0) 'transfer errors or host drops'
    # Metadata sidecar: capture header, at least one boundary, end record.
    $meta = @(Get-Content -LiteralPath ($Path + '.metadata.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
    Assert ($meta.Count -ge 3) "metadata has $($meta.Count) lines"
    Assert ($meta[0].type -eq 'capture' -and $meta[0].format -eq $Format) 'first metadata line is not the capture header'
    Assert ($meta[0].config.sample_rate -eq $captureRate) 'capture header sample rate'
    $boundaries = @($meta | Where-Object { $_.type -eq 'boundary' })
    Assert ($boundaries.Count -ge 1 -and $boundaries[0].file_byte -eq 0) 'first boundary must start at byte 0'
    foreach ($b in $boundaries) { Assert ($b.file_byte -eq $b.file_sample * $frame -and $b.file_byte -le $size) 'boundary offset outside file' }
    Assert ($meta[-1].type -eq 'end' -and $meta[-1].stats.samples_per_channel -eq $samples) 'end record mismatch'
    Set-Detail ('{0:N1} MiB, {1:N0} samples/ch, {2} boundary record(s)' -f ($size / 1MB), $samples, $boundaries.Count)
    return $r
}
Test-Case capture 'RX sc16 two-channel capture with metadata' {
    $null = Test-Capture $captureSc16 'sc16' '1,2' 1 4
    # sc16 is 12-bit left aligned: the low nibble of every sample is zero,
    # and live RX noise is not all zeros.
    $head = Read-Head $captureSc16 65536
    $nonzero = 0; $misaligned = 0
    for ($i = 0; $i -lt $head.Length; $i += 2) {
        $v = [BitConverter]::ToInt16($head, $i)
        if ($v -ne 0) { $nonzero++ }
        if (($v -band 0xF) -ne 0) { $misaligned++ }
    }
    Assert ($misaligned -eq 0) "$misaligned samples are not 12-bit left aligned"
    Assert ($nonzero -gt 0) 'capture contains only zeros'
}
Test-Case capture 'RX fc32 single-channel capture is normalised' {
    $path = New-Path 'capture-ch1.fc32'
    $null = Test-Capture $path 'fc32' '1' 0.5 8
    $head = Read-Head $path 65536
    for ($i = 0; $i -lt $head.Length; $i += 4) {
        $v = [BitConverter]::ToSingle($head, $i)
        Assert (-not [double]::IsNaN($v) -and [math]::Abs($v) -le 1.0) "sample $v outside [-1, 1]"
    }
}
Test-Case capture 'RX packed12 capture on channel 2' {
    $null = Test-Capture (New-Path 'capture-ch2.p12') 'packed12' '2' 0.5 3
}
Test-Case capture 'existing capture is protected without --overwrite' {
    if (!(Test-Path -LiteralPath $captureSc16)) { Skip-Test 'needs the sc16 capture' }
    $before = (Get-Item -LiteralPath $captureSc16).LastWriteTimeUtc
    $run = Invoke-Och @('rx', '--file', $captureSc16, '--seconds', '0.2', '--json')
    Assert-Exit $run 2
    Assert ((Get-Item -LiteralPath $captureSc16).LastWriteTimeUtc -eq $before) 'capture file was modified'
}

# ---------------------------------------------------------------------------
# playback
# ---------------------------------------------------------------------------
Test-Case playback 'TX plays an sc16 capture to EOF' {
    if ($SkipTx) { Skip-Test '-SkipTx' }
    if (!(Test-Path -LiteralPath $captureSc16)) { Skip-Test 'needs the sc16 capture' }
    $samples = (Get-Item -LiteralPath $captureSc16).Length / 8
    $run = Invoke-Och (@('tx', '--file', $captureSc16, '--format', 'sc16', '--channels', '1,2', '--sample-rate', "$captureRate", '--json') + $tx) -TimeoutSec 120
    Assert-Exit $run 0
    $r = From-Json $run.Stdout
    Assert ($r.tx.samples_per_channel -ge $samples) "accepted $($r.tx.samples_per_channel) of $samples samples/ch"
    Assert ($r.tx.transfer_errors -eq 0) 'transfer errors'
    $played = $r.tx.elapsed_seconds; $nominal = $samples / $captureRate
    Assert ($played -ge 0.8 * $nominal) ('stopped after {0:N2} s, file is {1:N2} s' -f $played, $nominal)
    # A finite file drains the device FIFO after EOF; tail underflow there is
    # documented behaviour and only reported, not judged.
    $tail = if ($r.tx.device_underflows -gt 0 -or $r.tx.tx_starvation_events -gt 0) { ', tail underflow at EOF' } else { '' }
    Set-Detail ('{0:N0} samples/ch in {1:N2} s, padding {2}{3}' -f $r.tx.samples_per_channel, $played, $r.tx.tx_padding_samples, $tail)
}
Test-Case playback 'TX loops a file for a fixed duration' {
    if ($SkipTx) { Skip-Test '-SkipTx' }
    if (!(Test-Path -LiteralPath $captureSc16)) { Skip-Test 'needs the sc16 capture' }
    $samples = (Get-Item -LiteralPath $captureSc16).Length / 8
    $run = Invoke-Och (@('tx', '--file', $captureSc16, '--loop', '--seconds', '3', '--sample-rate', "$captureRate", '--json') + $tx) -TimeoutSec 60
    Assert-Exit $run 0
    $r = From-Json $run.Stdout
    Assert ($r.tx.samples_per_channel -gt $samples) 'loop did not repeat the file'
    Assert ($r.tx.elapsed_seconds -ge 2.5 -and $r.tx.elapsed_seconds -lt 6) "elapsed $($r.tx.elapsed_seconds) s"
    Assert ($r.tx.device_underflows -eq 0 -and $r.tx.transfer_errors -eq 0) "underflows=$($r.tx.device_underflows)"
    Set-Detail ('{0:N1}x file length' -f ($r.tx.samples_per_channel / $samples))
}
Test-Case playback 'TX generated 1 MHz tone' {
    if ($SkipTx) { Skip-Test '-SkipTx' }
    $run = Invoke-Och (@('tx', '--tone', '1M', '--amplitude', '0.25', '--channels', '1', '--seconds', '2', '--json') + $tx) -TimeoutSec 60
    Assert-Exit $run 0
    $src = Json-Lines $run.Stderr | Where-Object { $_.type -eq 'tx_source' } | Select-Object -First 1
    Assert ($null -ne $src -and [double]$src.actual_tone_Hz -eq 1000000) 'tone frequency not reported as exactly 1 MHz'
    $r = From-Json $run.Stdout
    Assert ($r.tx.device_underflows -eq 0 -and $r.tx.transfer_errors -eq 0) 'underflow or transfer error'
}
Test-Case playback 'TX refuses a file shorter than the prefill' {
    $short = New-Path 'short.sc16'
    [IO.File]::WriteAllBytes($short, (New-Object byte[] 1024))
    $run = Invoke-Och @('tx', '--file', $short, '--json')
    Assert-Exit $run 2
    Assert ($run.Stderr -match 'too short') 'unexpected error text'
}

# ---------------------------------------------------------------------------
# bench (throughput)
# ---------------------------------------------------------------------------
Test-Case bench 'RX 2ch @ 61.44 MSPS (12-bit)' { Measure-Bench 'RX 2ch 61.44M' @('--rx', '--sample-rate', '61.44M', '--channels', '1,2') 61440000 2 0 3 }
Test-Case bench 'RX 1ch @ 61.44 MSPS (12-bit)' { Measure-Bench 'RX 1ch 61.44M' @('--rx', '--sample-rate', '61.44M', '--channels', '1') 61440000 1 0 3 }
Test-Case bench 'TX 2ch @ 61.44 MSPS (12-bit)' {
    if ($SkipTx) { Skip-Test '-SkipTx' }
    Measure-Bench 'TX 2ch 61.44M' (@('--tx', '--sample-rate', '61.44M', '--channels', '1,2') + $tx) 61440000 0 2 3
}
Test-Case bench 'RX+TX 2ch @ 30.72 MSPS (12-bit)' {
    if ($SkipTx) { Skip-Test '-SkipTx' }
    Measure-Bench 'Duplex 2ch 30.72M' (@('--rx', '--tx', '--sample-rate', '30.72M', '--channels', '1,2') + $tx) 30720000 2 2 3
}
Test-Case bench 'RX+TX 1ch @ 61.44 MSPS (12-bit)' {
    if ($SkipTx) { Skip-Test '-SkipTx' }
    Measure-Bench 'Duplex 1ch 61.44M' (@('--rx', '--tx', '--sample-rate', '61.44M', '--channels', '1') + $tx) 61440000 1 1 3
}
Test-Case bench 'RX+TX 2ch @ 50 MSPS (8-bit wire)' {
    if ($SkipTx) { Skip-Test '-SkipTx' }
    Measure-Bench 'Duplex 2ch 50M IQ8' (@('--rx', '--tx', '--iq-bits', '8', '--sample-rate', '50M', '--channels', '1,2') + $tx) 50000000 2 2 2
}

# ---------------------------------------------------------------------------
# output (human-readable text)
# ---------------------------------------------------------------------------
function Assert-PlainText([string]$Text, [string]$What) {
    Assert ($Text.Length -gt 0) "$What printed nothing"
    Assert ($Text.IndexOf([char]27) -lt 0) "$What contains ANSI escapes while redirected"
    foreach ($line in ($Text -split "`r?`n")) {
        Assert ($line.Length -le 78) "$What line wider than 78 columns: $line"
        Assert ($line -notmatch '[^\x00-\x7F]') "$What contains non-ASCII text while redirected: $line"
    }
}
Test-Case output 'redirected info/status/help are plain ASCII within 78 columns' {
    foreach ($c in @(@('info'), @('get', 'system.status'), @('clock', 'status'), @('fir', 'status'), @('find'))) {
        $run = Invoke-Och $c; Assert-Exit $run 0
        Assert-PlainText $run.Stdout ($c -join ' ')
    }
    $help = Invoke-Och @('help') -NoSerial; Assert-Exit $help 0
    Assert-PlainText $help.Stdout 'help'
    Assert ($help.Stdout -match 'Stream options' -and $help.Stdout -match '--no-color') 'help sections missing'
}
Test-Case output 'redirected bench prints summary and throughput table' {
    $run = Invoke-Och @('bench', '--rx', '--sample-rate', '30.72M', '--seconds', '1')
    Assert-Exit $run 0
    Assert-PlainText $run.Stdout 'bench'
    foreach ($needle in @('STREAM BENCHMARK', 'RF CONFIGURATION', 'STREAM COMPLETE', 'USB MB/s', 'RX STREAM SUMMARY', 'Faults')) {
        Assert ($run.Stdout -match [regex]::Escape($needle)) "missing '$needle'"
    }
    Assert ($run.Stderr -match '\[RUN \]' -and $run.Stderr -match '\[STOP\]') 'missing run/stop status on stderr'
}
Test-Case output '--json output carries no text decoration' {
    $run = Invoke-Och @('info', '--json'); Assert-Exit $run 0
    Assert ($run.Stdout.Trim().StartsWith('{') -and $run.Stdout.IndexOf([char]27) -lt 0) 'JSON stdout polluted'
}

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------
$pass = @($results | Where-Object Status -eq 'PASS').Count
$warn = @($results | Where-Object Status -eq 'WARN').Count
$fail = @($results | Where-Object Status -eq 'FAIL').Count
$skip = @($results | Where-Object Status -eq 'SKIP').Count

if ($benchRows.Count) {
    Write-Host ''
    Write-Host '  Throughput' -ForegroundColor Cyan
    $benchRows | Format-Table Test, Dir, Channels, MSPS, Seconds, MBps, Expected, Ratio, Overflows, Underflows, HostDrops, TransferErrors -AutoSize |
        Out-String -Width 200 | ForEach-Object { $_.TrimEnd() } | Write-Host
}
Write-Host ''
$summary = "  $($results.Count) tests: $pass passed, $warn warnings, $fail failed, $skip skipped"
Write-Host $summary -ForegroundColor $(if ($fail) { 'Red' } elseif ($warn) { 'Yellow' } else { 'Green' })

$report = [pscustomobject]@{
    date = (Get-Date).ToString('s'); och = $Och; version = $script:expectedVersion; serial = $Serial
    bench_seconds = $BenchSeconds; tx_attenuation_db = $TxAttenuation; skip_tx = [bool]$SkipTx
    environment = $environment
    summary = @{ total = $results.Count; pass = $pass; warn = $warn; fail = $fail; skip = $skip }
    tests = $results; throughput = $benchRows
}
$reportPath = Join-Path $OutDir 'cli-hardware-report.json'
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host "  Report: $reportPath"

if (-not $KeepFiles) {
    Get-ChildItem -LiteralPath $OutDir -File | Where-Object { $_.Name -ne 'cli-hardware-report.json' } | Remove-Item -Force
}
exit $(if ($fail) { 1 } else { 0 })
