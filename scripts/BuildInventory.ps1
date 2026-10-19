# Shared by build and packaging. Hash only the maintained build/test inputs,
# never work/, outputs/, local profiles, logs, or downloaded research material.
function Get-HubSourceInventory {
    param([Parameter(Mandatory)][string]$ProjectRoot)
    $root = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/')
    $inputs = @(Get-Item -LiteralPath (Join-Path $root 'CMakeLists.txt'))
    foreach ($folder in @('src', 'cmake', 'tests', 'tools', 'third_party')) {
        $inputs += Get-ChildItem -LiteralPath (Join-Path $root $folder) -Recurse -File
    }
    $inputs += Get-Item -LiteralPath (Join-Path $root 'scripts\build.ps1'), (Join-Path $root 'scripts\BuildInventory.ps1')
    @($inputs | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
            length = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    })
}

function Get-HubInventoryDigest {
    param([Parameter(Mandatory)][object[]]$Inventory)
    $lines = @($Inventory | ForEach-Object { '{0}:{1}' -f $_.path, $_.sha256 })
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
        ([System.BitConverter]::ToString($hash.ComputeHash($bytes))).Replace('-', '')
    } finally { $hash.Dispose() }
}
