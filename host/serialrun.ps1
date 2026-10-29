# Capture the board's TTL serial console, optionally driving it with a timed plan.
#   powershell -File build\host\serialrun.ps1 -Seconds 90 -Out build\board\ser.log
#   powershell -File build\host\serialrun.ps1 -Seconds 120 -Plan build\board\sweep.plan -Out out\sweep.log
# Plan lines are "<delay_ms>|<text>", same format as the telnet plans, so one
# script style covers both consoles.
param(
  [string]$PortName = 'COM5',
  [int]$Baud = 115200,
  [int]$Seconds = 60,
  [string]$Out = 'device\serial.log',
  [string]$Plan = '',
  [string]$Send = '',
  # Send a carriage return this often for the whole capture. The board's boot
  # stalls in the MT7916 probe unless somebody keeps touching the console, so
  # this reproduces that condition on a timer instead of by hand.
  [int]$Tick = 0,
  # What each tick sends. Empty = a bare carriage return (keeps boot going, and
  # is a no-op at prompts). Set to e.g. 'f' to hit OpenWrt's preinit failsafe key
  # repeatedly, since the failsafe window is short and its offset from the moment
  # we start capturing is unknown.
  [string]$TickText = ''
)

$ErrorActionPreference = 'Stop'
$port = New-Object System.IO.Ports.SerialPort $PortName, $Baud, 'None', 8, 'One'
$port.ReadTimeout = 200
$port.WriteTimeout = 200
$port.NewLine = "`r`n"
# Deliberately not asserting DTR/RTS: on some USB-TTL boards those are wired to
# reset/download, and a spurious pulse is not worth the risk.
$port.DtrEnable = $false
$port.RtsEnable = $false
$port.Open()

$items = @()
if ($Plan -and (Test-Path $Plan)) {
  foreach ($line in Get-Content $Plan) {
    if ($line -match '^(\d+)\|(.*)$') {
      $items += [pscustomobject]@{ At = [int]$matches[1]; Text = $matches[2]; Sent = $false }
    }
  }
}
if ($Send) { $items += [pscustomobject]@{ At = 1000; Text = $Send; Sent = $false } }
# Enter a few times: a BusyBox serial console only shows its prompt after input.
$items += [pscustomobject]@{ At = 2500; Text = ''; Sent = $false }

$start = Get-Date
$end = $start.AddSeconds($Seconds)
$dir = Split-Path -Parent $Out
if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
$sw = New-Object IO.StreamWriter($Out, $false, [Text.Encoding]::ASCII)
$sw.AutoFlush = $true

function Send-Item($text) {
  # "BREAK:x" raises the break condition the way a real terminal does for
  # Magic SysRq, then sends x. A plain character stream cannot be used for
  # SysRq unless CONFIG_MAGIC_SYSRQ_SERIAL is on.
  if ($text -like 'BREAK:*') {
    $port.BreakState = $true
    Start-Sleep -Milliseconds 300
    $port.BreakState = $false
    $text = $text.Substring(6)
  }
  if ($text) { $port.Write($text + "`r`n") } else { $port.Write("`r`n") }
}

$nextTick = $Tick
try {
  while ((Get-Date) -lt $end) {
    $ms = [int]((Get-Date) - $start).TotalMilliseconds
    foreach ($it in $items) {
      if (-not $it.Sent -and $ms -ge $it.At) { Send-Item $it.Text; $it.Sent = $true }
    }
    if ($Tick -gt 0 -and $ms -ge $nextTick) {
      if ($TickText) { Send-Item $TickText } else { $port.Write("`r") }
      $nextTick = $ms + $Tick
    }
    try {
      $data = $port.ReadExisting()
      if ($data) { $sw.Write($data) }
    } catch [TimeoutException] { }
    Start-Sleep -Milliseconds 40
  }
} finally {
  $port.Close()
  $sw.Close()
}
Write-Output "captured $Out"
