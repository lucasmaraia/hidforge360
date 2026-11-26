# PC test battery for the Brook Wingman Converter XE 2 (0c12:0efa), to run before trying it on the Xbox 360.
#
# Run it in its own PowerShell window (the interactive steps ask for answers):
#   powershell -ExecutionPolicy Bypass -File tools\teste-xe2.ps1
# Automatic steps only (identification, descriptors, report rate, output route results):
#   powershell -ExecutionPolicy Bypass -File tools\teste-xe2.ps1 -Auto
# One step only: -Etapa ident | analog | botoes | vibra | flags | expira | cadencia | precisao (also: driver, dpad)
#
# The input report is expected to be the DualShock 4 USB one (ID 0x01, 64 bytes), the output one 0x05 (32 bytes).
# The result is saved to logs\teste-xe2-<date>.txt. Prompts and log lines are in Portuguese.

param([switch]$Auto, [string]$Etapa = 'todas', [ValidateSet('xe2','dualsense')][string]$Pad = 'xe2')

. "$PSScriptRoot\hidlib.ps1"
$ErrorActionPreference = 'Stop'

# -Pad dualsense: a DualSense on a cable (054c:0ce6), to compare in the 'precisao' step. The sticks
# sit in the same bytes [1..4] of report 0x01 on both.
if ($Pad -eq 'dualsense') { $VID = 0x054C; $PRODUCT = 0x0CE6 } else { $VID = 0x0C12; $PRODUCT = 0x0EFA }
$EIXOS = @('LX','LY','RX','RY','L2a','R2a')
$log = New-Object System.Collections.Generic.List[string]
$resumo = [ordered]@{}

function Say([string]$s) { Write-Host $s; $log.Add($s) }
function Hex($bytes) { ($bytes | ForEach-Object { '{0:x2}' -f $_ }) -join ' ' }
function Want([string]$nome) { $Etapa -eq 'todas' -or $Etapa -eq $nome }
function Ask([string]$pergunta) {
  if ($Auto) { return '?' }
  $r = Read-Host "$pergunta [s/n]"
  if ($r -match '^[sSyY]') { 's' } else { 'n' }
}

function Save-Log([string]$sufixo = '') {
  $dir = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs'
  $nome = "teste-xe2-" + $(if ($Etapa -eq 'precisao') { "precisao-$Pad-" } else { '' })
  $file = Join-Path $dir ($nome + (Get-Date -Format 'yyyyMMdd-HHmmss') + $(if ($Auto) { '-auto' } else { '' }) + $sufixo + ".txt")
  [IO.File]::WriteAllLines($file, $log, (New-Object Text.UTF8Encoding($false)))
  Write-Host "log salvo em $file"
}

# Any error: show where, switch the motors off, keep what was measured so far.
trap {
  Say ("ERRO na linha {0}: {1}" -f $_.InvocationInfo.ScriptLineNumber, $_.Exception.Message)
  Say ("  " + $_.InvocationInfo.Line.Trim())
  if ($hr) { try { [void](Send-Out (New-Out 0x03 0 0 0 0 0) 'saida'); [void][Hid]::CloseHandle($hr) } catch {} }
  Save-Log '-erro'
  if (-not $Auto) { Read-Host "Enter para fechar" | Out-Null }
  exit 1
}

$dev = Get-HidPad -Vid $VID -ProductId $PRODUCT -Usage 5
if (-not $dev) { Say ("controle ({0:x4}:{1:x4}) nao encontrado. Esta plugado? (XE 2: modo PS4/PC)" -f $VID, $PRODUCT); exit 1 }

$hr = [Hid]::Open($dev.Path, $true)
if ($hr -eq [IntPtr]::Zero -or $hr -eq [IntPtr]-1) {
  Say ("abertura para leitura/escrita falhou, erro " + [Runtime.InteropServices.Marshal]::GetLastWin32Error()); exit 1
}
$IN = [int]$dev.In; $OUT = [int]$dev.Out

function Read-Report {
  $b = New-Object byte[] $IN; $n = 0
  if (-not [Hid]::ReadFile($hr, $b, $IN, [ref]$n, [IntPtr]::Zero)) { return $null }
  return ,$b
}

# Decodes the DualShock 4 USB layout.
function Decode($b) {
  $hat = $b[5] -band 0x0F
  [pscustomobject]@{
    LX = $b[1]; LY = $b[2]; RX = $b[3]; RY = $b[4]; L2a = $b[8]; R2a = $b[9]
    Hat = $hat
    Square = ($b[5] -band 0x10) -ne 0; Cross = ($b[5] -band 0x20) -ne 0
    Circle = ($b[5] -band 0x40) -ne 0; Triangle = ($b[5] -band 0x80) -ne 0
    L1 = ($b[6] -band 0x01) -ne 0; R1 = ($b[6] -band 0x02) -ne 0
    L2 = ($b[6] -band 0x04) -ne 0; R2 = ($b[6] -band 0x08) -ne 0
    Share = ($b[6] -band 0x10) -ne 0; Options = ($b[6] -band 0x20) -ne 0
    L3 = ($b[6] -band 0x40) -ne 0; R3 = ($b[6] -band 0x80) -ne 0
    PS = ($b[7] -band 0x01) -ne 0; Touch = ($b[7] -band 0x02) -ne 0
    Counter = $b[7] -shr 2
  }
}

function New-Out([byte]$flags, [int]$weak, [int]$strong, [int]$r, [int]$g, [int]$bl) {
  $buf = New-Object byte[] $OUT
  $buf[0] = 0x05; $buf[1] = $flags
  $buf[4] = [byte]$weak; $buf[5] = [byte]$strong
  $buf[6] = [byte]$r; $buf[7] = [byte]$g; $buf[8] = [byte]$bl
  return ,$buf
}
function Send-Out($buf, [string]$rota) {
  if ($rota -eq 'controle') { return [Hid]::HidD_SetOutputReport($hr, $buf, $buf.Length) }
  $w = 0; return [Hid]::WriteFile($hr, $buf, $buf.Length, [ref]$w, [IntPtr]::Zero)
}
function Stop-All { [void](Send-Out (New-Out 0x03 0 0 0 0 0) 'saida') }
function Hold($buf, [string]$rota, [int]$ms) {
  $ok = $true; $err = 0
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $ms) {
    if (-not (Send-Out $buf $rota)) { $ok = $false; $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
    Start-Sleep -Milliseconds 200
  }
  return [pscustomobject]@{ Ok = $ok; Erro = $err }
}

Say ("=== teste XE 2 - " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " ===")

# ---------------------------------------------------------------- 1. identification
if (Want 'ident') {
  Say "`n--- 1. identificacao ---"
  Say ("{0} / {1}  0c12:{2} ver {3}" -f $dev.Fabricante, $dev.Produto, $dev.PID, $dev.Versao)
  Say ("gamepad: in={0} out={1} feature={2}" -f $IN, $OUT, $dev.Feature)
  Say ("caminho: " + $dev.Path)
  $vend = Get-HidPad -Vid $VID -ProductId $PRODUCT -Usage 0x40
  if ($vend) { Say ("colecao vendor {0:x4}: feature={1}" -f $vend.UsagePage, $vend.Feature) }

  $usb = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^USB\\VID_0C12&PID_0EFA\\' } | Select-Object -First 1
  if ($usb) {
    $loc = (Get-PnpDeviceProperty -InstanceId $usb.InstanceId -KeyName DEVPKEY_Device_LocationInfo).Data
    $parent = (Get-PnpDeviceProperty -InstanceId $usb.InstanceId -KeyName DEVPKEY_Device_Parent).Data
    $port = if ($loc -match 'Port_#(\d+)') { [int]$Matches[1] } else { 0 }
    $hub = '\\?\' + ($parent -replace '\\', '#') + '#{f18a0e88-c30c-11d0-8815-00a0c906bed8}'
    Say ("USB: $loc, hub $parent")
    $dd = [Hid]::UsbDescriptor($hub, $port, 1, 18)
    if ($dd) {
      Say ("descritor de device: " + (Hex $dd))
      Say ("  bcdUSB {0:x2}{1:x2}  classe {2:x2}  bMaxPacketSize0 {3}  configs {4}" -f $dd[3], $dd[2], $dd[4], $dd[7], $dd[17])
    }
    $head = [Hid]::UsbDescriptor($hub, $port, 2, 9)
    if ($head) {
      $total = $head[2] + 256 * $head[3]
      $cfg = [Hid]::UsbDescriptor($hub, $port, 2, $total)
      Say ("descritor de configuracao ($total bytes): " + (Hex $cfg))
      $i = 0; $nIf = 0; $outEp = $null; $inEp = $null
      while ($i + 1 -lt $cfg.Length -and $cfg[$i] -gt 0) {
        $len = $cfg[$i]; $t = $cfg[$i + 1]
        switch ($t) {
          2 { Say ("  config: {0} interface(s), atributos {1:x2}, {2} mA" -f $cfg[$i+4], $cfg[$i+7], (2 * $cfg[$i+8])) }
          4 { $nIf++; Say ("  interface {0} alt {1}: classe {2:x2}/{3:x2}/{4:x2}, {5} endpoint(s)" -f $cfg[$i+2], $cfg[$i+3], $cfg[$i+5], $cfg[$i+6], $cfg[$i+7], $cfg[$i+4]) }
          0x21 { Say ("  HID: bcdHID {0:x2}{1:x2}, descritor de relatorio {2} bytes" -f $cfg[$i+3], $cfg[$i+2], ($cfg[$i+7] + 256 * $cfg[$i+8])) }
          5 {
            $addr = $cfg[$i+2]; $mps = ($cfg[$i+4] + 256 * $cfg[$i+5]) -band 0x7FF
            $dir = if ($addr -band 0x80) { 'IN' } else { 'OUT' }
            if ($dir -eq 'IN') { $inEp = $addr } else { $outEp = $addr }
            Say ("  endpoint {0:x2} {1}: tipo {2}, pacote {3}, intervalo {4}" -f $addr, $dir, ($cfg[$i+3] -band 3), $mps, $cfg[$i+6])
          }
        }
        $i += $len
      }
      $resumo['interfaces'] = $nIf
      $resumo['endpoint OUT'] = if ($null -ne $outEp) { '{0:x2}' -f $outEp } else { 'NAO TEM' }
    } else { Say "descritor de configuracao: falhou (hub $hub porta $port)" }
  }

  $b = Read-Report
  if ($b) {
    Say ("relatorio de entrada: " + (Hex $b[0..31]))
    $resumo['ID de entrada'] = '{0:x2}' -f $b[0]
    $okFmt = ($b[0] -eq 0x01 -and $b.Length -eq 64)
    Say ("  formato DS4 USB (ID 01, 64 bytes): " + $(if ($okFmt) { 'OK' } else { 'FALHOU' }))
    $resumo['formato DS4'] = if ($okFmt) { 'OK' } else { 'FALHOU' }
  }
}

# ---------------------------------------------------------------- 7. report rate (automatic, runs early)
if (Want 'cadencia') {
  Say "`n--- 7. cadencia de entrada (3 s) ---"
  $n = 0; $saltos = 0; $last = -1
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt 3000) {
    $b = Read-Report; if (-not $b) { break }
    $n++; $c = $b[7] -shr 2
    if ($last -ge 0 -and $c -ne (($last + 1) % 64)) { $saltos++ }
    $last = $c
  }
  $hz = [math]::Round($n / 3.0)
  Say ("  $n relatorios em 3 s = $hz por segundo; contador com $saltos salto(s)")
  $resumo['cadencia'] = "$hz/s"
}

# ---------------------------------------------------------------- 2. sticks and triggers
if ((Want 'analog') -and -not $Auto) {
  Say "`n--- 2. analogicos e gatilhos ---"
  Read-Host "Solte TUDO (analogicos no centro, gatilhos soltos) e aperte Enter" | Out-Null
  $rest = @{}; foreach ($k in 'LX','LY','RX','RY','L2a','R2a') { $rest[$k] = New-Object System.Collections.Generic.List[int] }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt 1500) { $d = Decode (Read-Report); foreach ($k in $EIXOS) { $rest[$k].Add([int]$d.$k) } }
  foreach ($k in 'LX','LY','RX','RY','L2a','R2a') {
    $m = $rest[$k] | Measure-Object -Minimum -Maximum -Average
    Say ("  repouso {0,-3}: media {1,5:N1}  faixa {2}-{3}" -f $k, $m.Average, $m.Minimum, $m.Maximum)
  }
  Read-Host "Agora gire os DOIS analogicos ate o fim, varias voltas, e aperte L2 e R2 ate o fundo. Enter para comecar (8 s)" | Out-Null
  $min = @{}; $max = @{}; foreach ($k in 'LX','LY','RX','RY','L2a','R2a') { $min[$k] = 255; $max[$k] = 0 }
  $sw = [Diagnostics.Stopwatch]::StartNew(); $tick = 0
  while ($sw.ElapsedMilliseconds -lt 8000) {
    $d = Decode (Read-Report)
    foreach ($k in $EIXOS) { if ($d.$k -lt $min[$k]) { $min[$k] = $d.$k }; if ($d.$k -gt $max[$k]) { $max[$k] = $d.$k } }
    if (++$tick % 25 -eq 0) { Write-Host -NoNewline ("`r  LX {0,3} LY {1,3}  RX {2,3} RY {3,3}  L2 {4,3} R2 {5,3}   " -f $d.LX, $d.LY, $d.RX, $d.RY, $d.L2a, $d.R2a) }
  }
  Write-Host ""
  $falha = $false
  foreach ($k in 'LX','LY','RX','RY','L2a','R2a') {
    $ok = ($min[$k] -le 5 -and $max[$k] -ge 250)
    if (-not $ok) { $falha = $true }
    Say ("  {0,-3}: {1,3} .. {2,3}  {3}" -f $k, $min[$k], $max[$k], $(if ($ok) { 'OK' } else { 'FAIXA CURTA' }))
  }
  $resumo['analogicos'] = if ($falha) { 'FAIXA CURTA (ver log)' } else { 'OK' }
}

# ---------------------------------------------------------------- 3. buttons
if ((Want 'botoes') -and -not $Auto) {
  Say "`n--- 3. botoes (aperte e solte cada um quando pedir; 10 s cada) ---"
  $hatNames = 'cima','cima-direita','direita','baixo-direita','baixo','baixo-esquerda','esquerda','cima-esquerda'
  $lista = @()
  for ($i = 0; $i -lt 8; $i++) { $lista += [pscustomobject]@{ Nome = "d-pad " + $hatNames[$i]; Hat = $i; Campo = $null } }
  foreach ($p in @(('X (cruz)','Cross'),('Bola','Circle'),('Quadrado','Square'),('Triangulo','Triangle'),
                   ('L1','L1'),('R1','R1'),('L2','L2'),('R2','R2'),('L3 (clique esquerdo)','L3'),('R3 (clique direito)','R3'),
                   ('Create/Share','Share'),('Options','Options'),('PS','PS'),('Clique do touchpad','Touch'))) {
    $lista += [pscustomobject]@{ Nome = $p[0]; Hat = -1; Campo = $p[1] }
  }
  $ok = 0; $faltou = @()
  foreach ($bt in $lista) {
    Write-Host -NoNewline ("  aperte {0,-22} " -f $bt.Nome)
    $hit = $false; $extra = $null
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 10000) {
      $d = Decode (Read-Report)
      $on = if ($bt.Hat -ge 0) { $d.Hat -eq $bt.Hat } else { $d.($bt.Campo) }
      if ($on) { $hit = $true; break }
    }
    if ($hit) {
      $ok++; Say ("  {0,-22} OK" -f $bt.Nome)
      while ($true) { $d = Decode (Read-Report); $on = if ($bt.Hat -ge 0) { $d.Hat -ne 8 } else { $d.($bt.Campo) }; if (-not $on) { break } }
      Start-Sleep -Milliseconds 150
    } else { $faltou += $bt.Nome; Say ("  {0,-22} NAO VISTO" -f $bt.Nome) }
  }
  $resumo['botoes'] = "$ok/$($lista.Count)" + $(if ($faltou) { " (faltou: " + ($faltou -join ', ') + ")" } else { '' })
}

# ---------------------------------------------------------------- 4. rumble over both routes
if (Want 'vibra') {
  Say "`n--- 4. vibracao e barra de luz, pelas duas rotas ---"
  Stop-All; Start-Sleep -Milliseconds 500
  $casos = @(
    @{ Nome = 'forte (esquerdo)'; Weak = 0;   Strong = 255; Cor = @(255,0,0);   CorNome = 'VERMELHO' },
    @{ Nome = 'fraco (direito)';  Weak = 255; Strong = 0;   Cor = @(0,255,0);   CorNome = 'VERDE' },
    @{ Nome = 'os dois';          Weak = 255; Strong = 255; Cor = @(0,0,255);   CorNome = 'AZUL' }
  )
  foreach ($rota in 'controle','saida') {
    $desc = if ($rota -eq 'controle') { 'HidD_SetOutputReport = SET_REPORT no endpoint de controle' } else { 'WriteFile = endpoint de saida (interrupt OUT)' }
    Say "  rota $rota ($desc)"
    $vibrou = 0; $acendeu = 0
    foreach ($c in $casos) {
      if (-not $Auto) { Write-Host "    -> $($c.Nome), barra $($c.CorNome), 2 s" }
      $r = Hold (New-Out 0x03 $c.Weak $c.Strong $c.Cor[0] $c.Cor[1] $c.Cor[2]) $rota 2000
      Stop-All
      $v = Ask "    vibrou ($($c.Nome))?"
      $l = Ask "    a barra do DualSense ficou $($c.CorNome)?"
      if ($v -eq 's') { $vibrou++ }; if ($l -eq 's') { $acendeu++ }
      Say ("    {0,-17} retorno ok={1} erro={2}  vibrou={3} luz={4}" -f $c.Nome, $r.Ok, $r.Erro, $v, $l)
      Start-Sleep -Milliseconds 600
    }
    $resumo["rota $rota"] = if ($Auto) { "retorno ok=$($r.Ok) erro=$($r.Erro)" } else { "vibrou $vibrou/3, luz $acendeu/3" }
  }
}

# ---------------------------------------------------------------- 5. output flags
if ((Want 'flags') -and -not $Auto) {
  Say "`n--- 5. flags de saida (endpoint de saida, 2 s cada) ---"
  foreach ($f in @(@{F=0x01;N='0x01 so motor (luz em BRANCO no relatorio)'}, @{F=0x02;N='0x02 so luz (motores em 255 no relatorio)'}, @{F=0x03;N='0x03 motor+luz'})) {
    Write-Host "    -> flags $($f.N)"
    [void](Hold (New-Out $f.F 255 255 255 255 255) 'saida' 2000)
    Stop-All
    $v = Ask "    vibrou?"; $l = Ask "    a barra ficou BRANCA?"
    Say ("    flags {0:x2}: vibrou={1} luz={2}" -f $f.F, $v, $l)
    Start-Sleep -Milliseconds 600
  }
}

# ---------------------------------------------------------------- 8. the exact report the driver sends (only with -Etapa driver)
if ($Etapa -eq 'driver') {
  Say "`n--- 8. relatorio do driver: 0x03 + cor do slot, repetido (2 s cada, pausa de 2 s) ---"
  $casos = @(
    @{ N = 'A 0x03 os dois motores, azul A0 (slot 1, igual ao driver)'; F = 0x03; W = 255; S = 255; C = @(0,0,0xA0) },
    @{ N = 'B 0x03 os dois motores, branco';                           F = 0x03; W = 255; S = 255; C = @(255,255,255) },
    @{ N = 'C 0x01 os dois motores';                                   F = 0x01; W = 255; S = 255; C = @(0,0,0) },
    @{ N = 'D 0x03 os dois motores, azul A0 (repeticao do A)';         F = 0x03; W = 255; S = 255; C = @(0,0,0xA0) },
    @{ N = 'E 0x03 meia forca (128), vermelho A0 (slot 2)';            F = 0x03; W = 128; S = 128; C = @(0xA0,0,0) },
    @{ N = 'F 0x03 os dois motores, preto (cor 0,0,0)';                F = 0x03; W = 255; S = 255; C = @(0,0,0) }
  )
  $ok = 0
  foreach ($c in $casos) {
    Write-Host "    -> $($c.N)"
    [void](Hold (New-Out $c.F $c.W $c.S $c.C[0] $c.C[1] $c.C[2]) 'saida' 2000)
    Stop-All
    $v = Ask "    vibrou?"
    if ($v -eq 's') { $ok++ }
    Say ("    {0}: vibrou={1}" -f $c.N, $v)
    Start-Sleep -Milliseconds 2000
  }
  $resumo['relatorio driver'] = "vibrou $ok/$($casos.Count)"
}

# ---------------------------------------------------------------- 9. live d-pad (only with -Etapa dpad)
if ($Etapa -eq 'dpad') {
  Say "`n--- 9. d-pad ao vivo: passe por todas as 8 direcoes, devagar (20 s) ---"
  $hatNames = 'cima','cima-direita','direita','baixo-direita','baixo','baixo-esquerda','esquerda','cima-esquerda','solto'
  $visto = @{}
  $sw = [Diagnostics.Stopwatch]::StartNew(); $tick = 0
  while ($sw.ElapsedMilliseconds -lt 20000 -and $visto.Count -lt 9) {
    $b = Read-Report; $h = $b[5] -band 0x0F
    if ($h -le 8) { $visto[[int]$h] = $true }
    if (++$tick % 20 -eq 0) {
      $faltam = (0..7 | Where-Object { -not $visto.ContainsKey($_) } | ForEach-Object { $hatNames[$_] }) -join ', '
      Write-Host -NoNewline ("`r  agora: {0,-15} byte5={1:x2}  faltam: {2,-90}" -f $hatNames[[math]::Min($h,8)], $b[5], $faltam)
    }
  }
  Write-Host ""
  foreach ($i in 0..7) { Say ("  {0,-15} {1}" -f $hatNames[$i], $(if ($visto.ContainsKey($i)) { 'OK' } else { 'NAO VISTO' })) }
  $resumo['d-pad'] = "$(@(0..7 | Where-Object { $visto.ContainsKey($_) }).Count)/8"
}

# ---------------------------------------------------------------- 10. stick precision (only with -Etapa precisao)
if ($Etapa -eq 'precisao') {
  Say "`n--- 10. precisao dos analogicos ($Pad) ---"
  $AX = @{ LX = 1; LY = 2; RX = 3; RY = 4 }

  # 10a. at rest: noise and centre offset (the "drift")
  Read-Host "Solte os DOIS analogicos (nao encoste) e aperte Enter (5 s)" | Out-Null
  $hist = @{}; foreach ($k in $AX.Keys) { $hist[$k] = @{} }
  $n = 0; $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt 5000) {
    $b = Read-Report; if (-not $b) { break }; $n++
    foreach ($k in $AX.Keys) { $v = [int]$b[$AX[$k]]; $hist[$k][$v] = 1 + [int]$hist[$k][$v] }
  }
  $hz = [math]::Round($n / 5.0)
  Say "  $n relatorios em 5 s ($hz/s)"
  $ruidoS = @{ R = 0; L = 0 }
  foreach ($k in 'LX','LY','RX','RY') {
    $vals = $hist[$k].Keys | Sort-Object
    $desv = ($vals | ForEach-Object { [math]::Abs($_ - 128) } | Measure-Object -Maximum).Maximum
    $lado = $k.Substring(0, 1)
    if ($desv -gt $ruidoS[$lado]) { $ruidoS[$lado] = $desv }
    $txt = ($vals | ForEach-Object { "{0}x{1}" -f $_, $hist[$k][$_] }) -join '  '
    Say ("  repouso {0}: desvio max do 128 = {1}   valores: {2}" -f $k, $desv, $txt)
  }
  $resumo['ruido repouso (dir.)'] = "$($ruidoS.R) contagem(ns) do 128"
  $resumo['ruido repouso (esq.)'] = "$($ruidoS.L) contagem(ns) do 128"

  $sticks = @(
    @{ nome = 'DIREITO';  tag = 'dir.'; x = 'RX'; y = 'RY'; bx = 3; by = 4; lado = 'R' },
    @{ nome = 'ESQUERDO'; tag = 'esq.'; x = 'LX'; y = 'LY'; bx = 1; by = 2; lado = 'L' })
  foreach ($st in $sticks) {
  $ruido = $ruidoS[$st.lado]
  Say "`n  -- analogico $($st.nome) --"

  # 10b. slow sweep: the adapter's own dead zone and skipped steps
  $dirs = @(
    @{ nome = 'DIREITA';  eixo = $st.x; sinal =  1 },
    @{ nome = 'ESQUERDA'; eixo = $st.x; sinal = -1 },
    @{ nome = 'BAIXO';    eixo = $st.y; sinal =  1 },
    @{ nome = 'CIMA';     eixo = $st.y; sinal = -1 })
  $saidas = @(); $gapMax = 0
  foreach ($d in $dirs) {
    Read-Host ("Analogico {0}: empurre MUITO devagar para a {1}, do centro ate o fim, em uns 6 s. Enter para comecar (8 s)" -f $st.nome, $d.nome) | Out-Null
    $vistos = @{}; $primeiro = $null; $seq = New-Object System.Collections.Generic.List[int]
    $sw = [Diagnostics.Stopwatch]::StartNew(); $tick = 0
    while ($sw.ElapsedMilliseconds -lt 8000) {
      $b = Read-Report; if (-not $b) { break }
      $v = [int]$b[$AX[$d.eixo]]; $o = ($v - 128) * $d.sinal
      if ($o -gt 0) { $vistos[$o] = $true; if ($null -eq $primeiro -and $o -gt $ruido) { $primeiro = $o } }
      if ($seq.Count -eq 0 -or $seq[$seq.Count - 1] -ne $v) { $seq.Add($v) }
      if (++$tick % 25 -eq 0) { Write-Host -NoNewline ("`r  {0} = {1,3}   " -f $d.eixo, $v) }
    }
    Write-Host ""
    $ks = @($vistos.Keys | Sort-Object)
    $topo = if ($ks.Count) { $ks[-1] } else { 0 }
    $pulados = 0; $gap = 0
    for ($i = 1; $i -lt $ks.Count; $i++) { $g = $ks[$i] - $ks[$i-1] - 1; $pulados += $g; if ($g -gt $gap) { $gap = $g } }
    if ($gap -gt $gapMax) { $gapMax = $gap }
    $saidas += $(if ($null -eq $primeiro) { 0 } else { $primeiro })
    Say ("  {0,-8}: primeiro valor fora do centro = {1} contagem(ns); chegou a {2}; {3} degraus distintos, {4} pulados (maior buraco {5})" -f `
      $d.nome, $primeiro, $topo, $ks.Count, $pulados, $gap)
    Say ("            sequencia: " + (($seq | Select-Object -First 40) -join ' ') + $(if ($seq.Count -gt 40) { ' ...' } else { '' }))
  }
  $resumo["saida do centro ($($st.tag))"] = ($saidas -join '/') + " contagens (dir/esq/baixo/cima)"
  $resumo["maior buraco ($($st.tag))"] = "$gapMax degrau(s)"

  # 10c. real update rate: reports between value CHANGES
  Read-Host "Gire o analogico $($st.nome) em circulos, sem parar, numa velocidade media. Enter para comecar (5 s)" | Out-Null
  $idx = 0; $ultimaMud = 0; $last = $null; $inter = New-Object System.Collections.Generic.List[int]
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt 5000) {
    $b = Read-Report; if (-not $b) { break }; $idx++
    $cur = ([int]$b[$st.bx] -shl 8) -bor [int]$b[$st.by]
    if ($null -ne $last -and $cur -ne $last) { $inter.Add($idx - $ultimaMud); $ultimaMud = $idx }
    $last = $cur
  }
  $hz2 = $idx / ($sw.ElapsedMilliseconds / 1000.0)
  if ($inter.Count -gt 2) {
    $ord = @($inter | Select-Object -Skip 1 | Sort-Object)
    $med = $ord[[int]($ord.Count / 2)]; $p90 = $ord[[int]($ord.Count * 0.9)]
    $msRel = 1000.0 / $hz2
    Say ("  {0} relatorios ({1:N0}/s), {2} mudancas; entre mudancas: mediana {3} relatorio(s) = {4:N1} ms, p90 {5} = {6:N1} ms" -f `
      $idx, $hz2, $inter.Count, $med, ($med * $msRel), $p90, ($p90 * $msRel))
    $resumo["atualizacao real ($($st.tag))"] = ("a cada {0:N1} ms (mediana), p90 {1:N1} ms" -f ($med * $msRel), ($p90 * $msRel))
  } else {
    Say "  quase nenhuma mudanca vista; o analogico foi girado?"
  }
  }
}

# ---------------------------------------------------------------- 6. how long one rumble report lasts
if ((Want 'expira') -and -not $Auto) {
  Say "`n--- 6. expiracao: um envio so, sem repetir ---"
  Read-Host "Segure o controle. Enter para mandar UM relatorio de vibracao; aperte Enter de novo quando o motor PARAR (ou depois de 10 s se nao parar)" | Out-Null
  [void](Send-Out (New-Out 0x03 255 255 0 0 0) 'saida')
  $sw = [Diagnostics.Stopwatch]::StartNew()
  Read-Host "  (Enter quando parar)" | Out-Null
  $t = $sw.ElapsedMilliseconds
  Stop-All
  Say ("  o motor ficou ligado ~{0} ms com um envio" -f $t)
  $resumo['expiracao'] = "$t ms (driver reenvia a cada 250 ms)"
}

Stop-All
[void][Hid]::CloseHandle($hr)

Say "`n=== resumo ==="
foreach ($k in $resumo.Keys) { Say ("  {0,-14} {1}" -f $k, $resumo[$k]) }

Save-Log
if (-not $Auto) { Read-Host "Enter para fechar" | Out-Null }
