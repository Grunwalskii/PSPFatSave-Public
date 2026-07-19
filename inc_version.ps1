$versionFile = Join-Path $PSScriptRoot "src\plugin\version.h"
$content = Get-Content $versionFile -Raw

if ($content -match '#define VERSION_NUMBER (\d+)') {
    $oldVersion = [int]$Matches[1]
    $newVersion = $oldVersion + 1
    $newContent = $content -replace '#define VERSION_NUMBER \d+', "#define VERSION_NUMBER $newVersion"
    $newContent = $newContent -replace '#define VERSION_STRING "\d+"', "#define VERSION_STRING `"$newVersion`""
    $Utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($versionFile, $newContent, $Utf8NoBom)
    Write-Output $newVersion
}
