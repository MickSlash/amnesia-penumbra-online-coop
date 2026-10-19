param([string]$Executable = "$PSScriptRoot/../../build/network/NetworkTests.exe", [int]$Port = 27029)
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Executable).Path
$logDir = [IO.Path]::GetDirectoryName($exePath)
& $exePath
if ($LASTEXITCODE -ne 0) { throw 'Protocol validation failed' }
function Test-Connection([int]$TestPort, [bool]$RejectVersion, [bool]$Malformed = $false) {
    $hostProcess = Start-Process -FilePath $exePath -ArgumentList 'host', $TestPort -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logDir/host-$TestPort.log" -RedirectStandardError "$logDir/host-$TestPort.err"
    # Windows PowerShell reports a null ExitCode unless the handle was opened early.
    $null = $hostProcess.Handle
    try {
        if ($RejectVersion) {
            & $exePath bad-version $TestPort
            if ($LASTEXITCODE -ne 0) { throw 'Version rejection failed' }
        }
        if ($Malformed) {
            & $exePath malformed $TestPort
            if ($LASTEXITCODE -ne 0) { throw 'Malformed gameplay rejection failed' }
        }
        $clientMode = if($Malformed) { 'client-resume' } else { 'client' }
        & $exePath $clientMode $TestPort
        if ($LASTEXITCODE -ne 0) { throw 'Client test failed' }
        if (!$hostProcess.WaitForExit(10000)) { throw 'Host did not terminate' }
        $hostProcess.Refresh()
        if ($hostProcess.ExitCode -ne 0) { throw 'Host test failed' }
        Get-Content -LiteralPath "$logDir/host-$TestPort.log"
    } finally {
        if (!$hostProcess.HasExited) { Stop-Process -Id $hostProcess.Id }
    }
}
Test-Connection $Port $false
Test-Connection ($Port + 1) $true
Test-Connection ($Port + 2) $false $true
