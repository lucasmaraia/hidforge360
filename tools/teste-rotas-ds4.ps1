. "$PSScriptRoot\hidlib.ps1"
$dev = Get-SonyPad
if (-not $dev) { "sem DS4"; exit }
"$($dev.Produto) [$($dev.PID)]  in=$($dev.In) out=$($dev.Out) feature=$($dev.Feature)"
$h = [Hid]::Open($dev.Path, $true)
if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr]-1) {
  "abertura para escrita falhou, erro " + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error(); exit
}

function New-Report($r, $g, $b, $strong, $weak) {
  $buf = New-Object byte[] 32
  $buf[0] = 0x05
  $buf[1] = 0x03
  $buf[4] = [byte]$weak
  $buf[5] = [byte]$strong
  $buf[6] = [byte]$r
  $buf[7] = [byte]$g
  $buf[8] = [byte]$b
  return $buf
}

"--- rota A: HidD_SetOutputReport  (SET_REPORT no endpoint de CONTROLE) -> VERDE, 3 s ---"
$okA = $null; $errA = 0
for ($i = 0; $i -lt 15; $i++) {
  $buf = New-Report 0 255 0 255 255
  $okA = [Hid]::HidD_SetOutputReport($h, $buf, 32)
  if (-not $okA) { $errA = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error() }
  Start-Sleep -Milliseconds 200
}
"  HidD_SetOutputReport: ok=$okA erro=$errA"

# switch everything off between the two routes, through the route known to work
$off = New-Report 0 0 0 0 0
$w = 0
[void][Hid]::WriteFile($h, $off, 32, [ref]$w, [IntPtr]::Zero)
Start-Sleep -Milliseconds 800

"--- rota B: WriteFile  (endpoint de SAIDA, interrupt OUT) -> AZUL, 3 s ---"
$okB = $null; $wrote = 0
for ($i = 0; $i -lt 15; $i++) {
  $buf = New-Report 0 0 255 255 255
  $okB = [Hid]::WriteFile($h, $buf, 32, [ref]$wrote, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 200
}
"  WriteFile: ok=$okB escritos=$wrote"

[void][Hid]::WriteFile($h, $off, 32, [ref]$w, [IntPtr]::Zero)
[void][Hid]::CloseHandle($h)
"fim"
