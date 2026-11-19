<#
.SYNOPSIS
    Saves MyRSM's "/" JSON payload as a parity fixture.

.DESCRIPTION
    Fetches the given MyRSM URL (default http://127.0.0.1:34567/) and writes the
    response bytes unchanged to tests/fixtures/captured-<Name>.json. The parity
    test (tools/run_parity.py) picks up every fixture automatically.

    The file contains that machine's sensor/hardware names: review it before
    committing.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/capture-fixture.ps1 -Name gaming-pc
#>
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Url = 'http://127.0.0.1:34567/'
)

$ErrorActionPreference = 'Stop'
if ($Name -notmatch '^[A-Za-z0-9_-]+$') {
    throw "Name may only contain letters, digits, '-' and '_'."
}

$fixtures = Join-Path (Split-Path -Parent $PSScriptRoot) 'tests\fixtures'
$out = Join-Path $fixtures "captured-$Name.json"

$resp = Invoke-WebRequest -Uri $Url -UseBasicParsing
$bytes = $resp.RawContentStream.ToArray()

# Validate: must be a non-empty top-level JSON array.
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
$parsed = $text | ConvertFrom-Json
if ($null -eq $parsed -or -not ($text.TrimStart().StartsWith('['))) {
    throw "Response from $Url is not a JSON array."
}

[System.IO.File]::WriteAllBytes($out, $bytes)
Write-Host "Saved $(@($parsed).Count) readings to $out"
