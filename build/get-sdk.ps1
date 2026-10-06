param([string]$Version = '1.0.4258.31')
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$sdk = Join-Path $PSScriptRoot 'sdk'
$include = Join-Path $sdk 'include'
$x64 = Join-Path $sdk 'x64'
if ((Test-Path (Join-Path $include 'WebView2.h')) -and (Test-Path (Join-Path $x64 'WebView2Loader.dll'))) {
    Write-Host 'WebView2 SDK already present.'
    exit 0
}
New-Item -ItemType Directory -Path $include, $x64 -Force | Out-Null

$tmp = Join-Path $sdk 'pkg'
$zip = Join-Path $sdk 'webview2.zip'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"
Write-Host "Downloading WebView2 SDK $Version"
Invoke-WebRequest -Uri $url -OutFile $zip -TimeoutSec 180
Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
Copy-Item (Join-Path $tmp 'build\native\include\*.h') $include -Force
Copy-Item (Join-Path $tmp 'build\native\x64\WebView2Loader.dll') $x64 -Force
Remove-Item $tmp -Recurse -Force
Remove-Item $zip -Force
if (-not (Test-Path (Join-Path $include 'WebView2.h'))) { throw 'WebView2.h not found in the package.' }
Write-Host "SDK ready: $sdk"
