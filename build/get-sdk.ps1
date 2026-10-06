param([string]$Version = '1.0.4258.31')
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$include = Join-Path $PSScriptRoot 'sdk\include'
if (Test-Path (Join-Path $include 'WebView2.h')) { Write-Host 'WebView2 SDK already present.'; exit 0 }
New-Item -ItemType Directory -Path $include -Force | Out-Null

$tmp = Join-Path $PSScriptRoot 'sdk\pkg'
$zip = Join-Path $PSScriptRoot 'sdk\webview2.zip'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"
Write-Host "Downloading WebView2 SDK $Version"
Invoke-WebRequest -Uri $url -OutFile $zip -TimeoutSec 180
Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
Copy-Item (Join-Path $tmp 'build\native\include\*.h') $include -Force
Remove-Item $tmp -Recurse -Force
Remove-Item $zip -Force
if (-not (Test-Path (Join-Path $include 'WebView2.h'))) { throw 'WebView2.h not found in the package.' }
Write-Host "SDK ready: $include"
