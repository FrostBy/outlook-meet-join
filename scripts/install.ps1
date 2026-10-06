param(
    [ValidateSet('Install', 'Uninstall', 'Status', 'UpdatePayload')]
    [string]$Action = 'Status'
)

$ErrorActionPreference = 'Stop'

$DllName   = 'OutlookMeetJoin.dll'
$DataDir   = Join-Path $env:LOCALAPPDATA 'OutlookMeetJoin'
$Payload   = Join-Path $DataDir 'inject.js'
$LogFile   = Join-Path $DataDir 'loader.log'
$StateFile = Join-Path $DataDir 'state.json'
$SystemDll = Join-Path $env:windir "System32\$DllName"
$Ifeo      = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\olk.exe'
$Aumid     = 'Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows'

function Ok   ([string]$t) { Write-Host "  [ OK ] $t" -ForegroundColor Green }
function Warn ([string]$t) { Write-Host "  [WARN] $t" -ForegroundColor Yellow }
function Info ([string]$t) { Write-Host "  [info] $t" -ForegroundColor DarkCyan }

function Find-Source([string]$name, [string[]]$dirs) {
    foreach ($d in $dirs) {
        $p = Join-Path $PSScriptRoot $d | Join-Path -ChildPath $name
        if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path }
    }
    return $null
}

function Assert-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator rights are required. Run install.cmd, it asks for elevation.'
    }
}

function Get-IfeoValue([string]$name) {
    if (-not (Test-Path -LiteralPath $Ifeo)) { return $null }
    $v = Get-ItemProperty -LiteralPath $Ifeo -Name $name -ErrorAction SilentlyContinue
    if ($null -eq $v) { return $null }
    return $v.$name
}

function Read-State {
    if (-not (Test-Path -LiteralPath $StateFile)) { return $null }
    try { return Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json } catch { return $null }
}

function Write-State($state) {
    New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
    [IO.File]::WriteAllText($StateFile, ($state | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
}

function Stop-Outlook {
    $procs = @(Get-Process -Name 'olk' -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { return }
    Info 'closing Outlook'
    $procs | Stop-Process -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 20 -and @(Get-Process -Name 'olk' -ErrorAction SilentlyContinue).Count -gt 0; $i++) { Start-Sleep -Milliseconds 500 }
}

function Start-Outlook {
    try { Start-Process "shell:AppsFolder\$Aumid"; Info 'starting Outlook' }
    catch { Warn 'could not start Outlook, open it manually' }
}

function Copy-Payload {
    $src = Find-Source 'inject.js' @('.', '..\src\payload', '..\dist')
    if (-not $src) { throw 'inject.js not found next to the script.' }
    New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
    Copy-Item -LiteralPath $src -Destination $Payload -Force
    Ok "payload copied to $Payload"
}

function Wait-Injection {
    Info 'waiting for Outlook to load (up to 60 s)'
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 1
        if ((Test-Path -LiteralPath $LogFile) -and (Select-String -LiteralPath $LogFile -Pattern 'ExecuteScript OK' -Quiet)) {
            Ok 'payload injected'
            return
        }
    }
    Warn "no injection logged yet, check $LogFile"
}

function Invoke-Install {
    Assert-Admin
    $dll = Find-Source $DllName @('.', '..\dist')
    if (-not $dll) { throw "$DllName not found. Build it with build\build.cmd or download a release." }

    Copy-Payload
    Stop-Outlook
    Copy-Item -LiteralPath $dll -Destination $SystemDll -Force
    Ok "DLL copied to $SystemDll"

    $current = Get-IfeoValue 'VerifierDlls'
    if ($current -and $current -ne $DllName) { Warn "replacing VerifierDlls = $current; Uninstall restores it" }
    if ($null -eq (Read-State)) {
        Write-State ([pscustomobject]@{
            ifeoExisted  = (Test-Path -LiteralPath $Ifeo)
            verifierDlls = Get-IfeoValue 'VerifierDlls'
            globalFlag   = Get-IfeoValue 'GlobalFlag'
            installedAt  = (Get-Date).ToString('s')
        })
    }

    if (-not (Test-Path -LiteralPath $Ifeo)) { New-Item -Path $Ifeo | Out-Null }
    Set-ItemProperty -LiteralPath $Ifeo -Name 'VerifierDlls' -Value $DllName
    Set-ItemProperty -LiteralPath $Ifeo -Name 'GlobalFlag' -Value 0x100 -Type DWord
    Ok 'registered in Image File Execution Options'

    Remove-Item -LiteralPath $LogFile -Force -ErrorAction SilentlyContinue
    Start-Outlook
    Wait-Injection
}

function Invoke-Uninstall {
    Assert-Admin
    Stop-Outlook
    $state = Read-State

    if ((Test-Path -LiteralPath $Ifeo) -and (Get-IfeoValue 'VerifierDlls') -eq $DllName) {
        if ($state -and $state.verifierDlls) { Set-ItemProperty -LiteralPath $Ifeo -Name 'VerifierDlls' -Value $state.verifierDlls }
        else { Remove-ItemProperty -LiteralPath $Ifeo -Name 'VerifierDlls' }
        if ($state -and $null -ne $state.globalFlag) { Set-ItemProperty -LiteralPath $Ifeo -Name 'GlobalFlag' -Value $state.globalFlag -Type DWord }
        else { Remove-ItemProperty -LiteralPath $Ifeo -Name 'GlobalFlag' -ErrorAction SilentlyContinue }
        $left = @((Get-Item -LiteralPath $Ifeo).Property).Count + @(Get-ChildItem -LiteralPath $Ifeo).Count
        if ($left -eq 0 -and -not ($state -and $state.ifeoExisted)) { Remove-Item -LiteralPath $Ifeo }
        Ok 'Image File Execution Options restored'
    } elseif (Get-IfeoValue 'VerifierDlls') {
        Warn 'VerifierDlls points to another DLL, left untouched'
    }

    if (Test-Path -LiteralPath $SystemDll) {
        try { Remove-Item -LiteralPath $SystemDll -Force; Ok 'DLL removed' }
        catch { Warn "could not remove $SystemDll, it is probably still loaded; reboot and run Uninstall again" }
    }
    Remove-Item -LiteralPath $DataDir -Recurse -Force -ErrorAction SilentlyContinue
    Ok "removed $DataDir"
    Start-Outlook
}

function Invoke-UpdatePayload {
    Copy-Payload
    Info 'new windows pick it up immediately; restart Outlook to update the main window'
}

function Show-Status {
    $outlook = Get-AppxPackage -Name 'Microsoft.OutlookForWindows' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($outlook) { Info "Outlook $($outlook.Version)" } else { Warn 'new Outlook is not installed' }

    $verifier = Get-IfeoValue 'VerifierDlls'
    if ($verifier -eq $DllName) { Ok "VerifierDlls = $verifier" }
    elseif ($verifier) { Warn "VerifierDlls = $verifier (not ours)" }
    else { Warn 'not registered in IFEO (an Outlook update can remove it; run Install again)' }

    if (Test-Path -LiteralPath $SystemDll) { Ok "DLL present: $SystemDll" } else { Warn 'DLL missing in System32' }
    if (Test-Path -LiteralPath $Payload) { Ok "payload present: $Payload" } else { Warn 'payload missing' }

    if (Test-Path -LiteralPath $LogFile) {
        Info 'last log lines:'
        Get-Content -LiteralPath $LogFile -Tail 8 -Encoding UTF8 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    }
}

switch ($Action) {
    'Install'       { Invoke-Install }
    'Uninstall'     { Invoke-Uninstall }
    'UpdatePayload' { Invoke-UpdatePayload }
    'Status'        { Show-Status }
}
