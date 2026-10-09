param([string]$OutputDirectory = (Join-Path $PSScriptRoot "../managed-memory-capture"))
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Only launches the original fixture project. No attach mode or arbitrary target is accepted.
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$fixture = Join-Path $PSScriptRoot "../tests/fixtures/managed_memory/ManagedMemory.csproj"
$app = Join-Path $output "app"
if (Test-Path $app) { throw "Use a fresh output directory to avoid stale fixture files" }
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.DiagnosticTools -property installationPath
if (!$installation) { throw "Visual Studio Diagnostic Tools are not installed" }
$collector = Join-Path $installation "Team Tools/DiagnosticsHub/Collector"
$diagnostics = Join-Path $collector "VSDiagnostics.exe"
$config = Join-Path $collector "AgentConfigs/DotNetObjectAllocBase.json"
if (!(Test-Path $diagnostics) -or !(Test-Path $config)) { throw "VSDiagnostics or DotNetObjectAllocBase.json is missing" }

& dotnet publish $fixture -c Release --self-contained false -r win-x64 -o $app *> (Join-Path $output "build.log")
if ($LASTEXITCODE -ne 0) { throw "Fixture build failed; see build.log" }
Copy-Item $config (Join-Path $output "DotNetObjectAllocBase.json")
$metadata = [ordered]@{
    collectedUtc = [DateTime]::UtcNow.ToString("o")
    diagnosticsVersion = (Get-Item $diagnostics).VersionInfo.FileVersion
    configSha256 = (Get-FileHash $config -Algorithm SHA256).Hash
    workload = "TraceManagedMemoryFixture"
    expectedRetainedPayloads = 2048
    expectedReleasedPayloads = 4096
    semantics = "Capture must be inspected for allocation, stack, movement and survival coverage; counts are not presumed exact."
}
$metadata | ConvertTo-Json | Set-Content (Join-Path $output "capture.json")

$session = 73
& $diagnostics start $session "/launch:$(Join-Path $app 'TraceManagedMemoryFixture.exe')" "/loadConfig:$config" *> (Join-Path $output "start.log")
if ($LASTEXITCODE -ne 0) { throw "VSDiagnostics start failed; see start.log" }
try {
    New-Item -ItemType File -Force (Join-Path $app "start-workload") | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (!(Test-Path (Join-Path $app "workload-complete"))) {
        if ([DateTime]::UtcNow -gt $deadline) { throw "Synthetic workload did not complete" }
        Start-Sleep -Milliseconds 200
    }
} finally {
    & $diagnostics stop $session "/output:$(Join-Path $output 'managed-allocation-survival.diagsession')" *> (Join-Path $output "stop.log")
    $stopCode = $LASTEXITCODE
    New-Item -ItemType File -Force (Join-Path $app "stop-workload") | Out-Null
    if ($stopCode -ne 0) { throw "VSDiagnostics stop failed; see stop.log" }
}
if (!(Test-Path (Join-Path $output "managed-allocation-survival.diagsession"))) { throw "No capture was produced" }
$checkpoints = Get-Content (Join-Path $app "checkpoints.json") -Raw | ConvertFrom-Json
if ($checkpoints.checkpoints[-1].retainedPayloads -ne 2048 -or $checkpoints.checkpoints[-1].releasedObjectAlive) {
    throw "Synthetic retention/release oracle failed"
}

# Keep Windows' interpretation alongside the capture as an independent decoder oracle.
$etl = Join-Path ([System.IO.Path]::GetTempPath()) ("trace-managed-" + [Guid]::NewGuid() + ".etl")
$archive = [System.IO.Compression.ZipFile]::OpenRead((Join-Path $output "managed-allocation-survival.diagsession"))
try {
    $entries = @($archive.Entries | Where-Object { $_.Name -eq "sc.user_aux.etl" })
    if ($entries.Count -ne 1) { throw "Expected one user ETL resource" }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $etl)
    & tracerpt $etl -of XML -o (Join-Path $output "events.xml") -export (Join-Path $output "schema.man") -summary (Join-Path $output "summary.xml") -y *> (Join-Path $output "tracerpt.log")
    if ($LASTEXITCODE -ne 0) { throw "Windows trace export failed; see tracerpt.log" }
} finally {
    $archive.Dispose()
    if (Test-Path $etl) { Remove-Item $etl }
}
Get-ChildItem $output -Recurse -File | Where-Object { $_.Extension -in ".diagsession", ".exe", ".dll", ".pdb" } |
    Get-FileHash -Algorithm SHA256 | Select-Object Hash, Path | ConvertTo-Json | Set-Content (Join-Path $output "hashes.json")
Write-Output "CAPTURE OK: $output"
