param(
    [string]$KeilRoot = 'D:\Application\Keil5',
    [switch]$Rebuild,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$compilerRoot = Join-Path $KeilRoot 'ARM\ARMCLANG\bin'
$uv4 = Join-Path $KeilRoot 'UV4\UV4.exe'
if (-not (Test-Path -LiteralPath $uv4)) { throw "Keil not found: $uv4. Specify -KeilRoot with your Keil installation directory." }
if (-not (Test-Path -LiteralPath (Join-Path $compilerRoot 'fromelf.exe'))) { throw "Arm Compiler 6 tools not found: $compilerRoot" }
if (-not (Get-Command $Python -ErrorAction SilentlyContinue)) { throw "Python not found: $Python. Install Python or specify -Python with its executable path." }
$buildDir = Join-Path $projectRoot 'Build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$hexPath = Join-Path $buildDir 'STM32F103C8T6.hex'
$binPath = Join-Path $buildDir 'STM32F103C8T6.bin'
# Preserve the previous firmware before Keil overwrites its fixed output names.
$previousImages = @($hexPath, $binPath) | Where-Object { Test-Path -LiteralPath $_ }
if ($previousImages) {
    $archiveDir = Join-Path $buildDir ('History\' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
    New-Item -ItemType Directory -Path $archiveDir | Out-Null
    $checksums = foreach ($previousImage in $previousImages) {
        $savedImage = Join-Path $archiveDir (Split-Path -Leaf $previousImage)
        Copy-Item -LiteralPath $previousImage -Destination $savedImage
        '{0}  {1}' -f (Get-FileHash -LiteralPath $savedImage -Algorithm SHA256).Hash, (Split-Path -Leaf $savedImage)
    }
    $checksums | Set-Content -LiteralPath (Join-Path $archiveDir 'SHA256SUMS.txt') -Encoding ASCII
    Write-Host "Previous firmware saved: $archiveDir"
}
$logPath = Join-Path $buildDir 'build.log'
Set-Content -LiteralPath $logPath -Value ''
function Invoke-Checked([string]$Tool, [string[]]$ToolArgs) {
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $result = & (Join-Path $compilerRoot $Tool) @ToolArgs 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    $result | Tee-Object -FilePath $logPath -Append
    if ($code -ne 0) { throw "$Tool failed ($code). See $logPath" }
}
$projectPath = Join-Path $projectRoot 'STM32F103C8T6.uvprojx'
$buildMode = if ($Rebuild -or -not (Test-Path -LiteralPath $hexPath)) { '-r' } else { '-b' }
$buildArgs = @($buildMode, ('"{0}"' -f $projectPath), '-j0', '-t', 'STM32F103C8T6_4Motor_AB', '-o', ('"{0}"' -f $logPath))
$buildProcess = Start-Process -FilePath $uv4 -ArgumentList $buildArgs -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru -Wait
Get-Content -LiteralPath $logPath | Write-Output
if ($buildProcess.ExitCode -ne 0) { throw "Keil build failed ($($buildProcess.ExitCode)). See $logPath" }
$image = Join-Path $buildDir 'STM32F103C8T6.axf'
if (-not (Test-Path -LiteralPath $image)) { throw "Build output not found: $image" }
if (-not (Test-Path -LiteralPath $hexPath)) { throw "HEX output not found: $hexPath. Enable Create HEX File in the Keil project." }
Invoke-Checked 'fromelf.exe' @('--bin','--output',$binPath,$image)
Invoke-Checked 'fromelf.exe' @('--text','-z',$image)
& $Python -B (Join-Path $PSScriptRoot 'verify_image.py')
if ($LASTEXITCODE -ne 0) { throw "HEX/BIN verification failed. See $logPath and the verification output above." }
Write-Host "HEX ready: $hexPath"
Write-Host "BIN ready: $binPath"
Write-Host "HEX SHA256: $((Get-FileHash -LiteralPath $hexPath -Algorithm SHA256).Hash)"
