# find_offsets.ps1 — searches Cpp2IL output for ROB RUN offsets
# Usage: .\find_offsets.ps1 -Path "C:\Users\dcact\Desktop\cpp2il_out"

param(
    [Parameter(Mandatory=$true)]
    [string]$Path
)

$classes = @(
    "BattleModel",
    "BattleEntitiesModel",
    "BattleCharacterModel",
    "BattleCharacterEntityModel",
    "PlayerEntityModel",
    "CharacterTransformModel",
    "IPrimitiveModel",
    "CharacterHealthModel",
    "BattleSlotsModel"
)

Write-Host "`n=== ROB RUN offset finder ===" -ForegroundColor Cyan
Write-Host "Scanning: $Path`n"

foreach ($cls in $classes) {
    Write-Host "--- $cls ---" -ForegroundColor Yellow
    $found = $false

    Get-ChildItem -Path $Path -Recurse -Filter "*.cs" -ErrorAction SilentlyContinue | ForEach-Object {
        $content = Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue
        if ($content -and $content -match "class\s+$cls") {
            $found = $true
            Write-Host "  File: $($_.FullName)" -ForegroundColor Green

            $lines = Get-Content $_.FullName
            $inClass = $false
            $braceDepth = 0
            foreach ($line in $lines) {
                if ($line -match "class\s+$cls") {
                    $inClass = $true
                    $braceDepth = 0
                }
                if ($inClass) {
                    if ($line -match '\{') { $braceDepth++ }
                    if ($line -match '\}') { $braceDepth--; if ($braceDepth -le 0) { $inClass = $false } }

                    if ($line -match 'FieldOffset.*Offset\s*=\s*"?(0x[0-9A-Fa-f]+)"?' -or
                        $line -match 'Offset:\s*(0x[0-9A-Fa-f]+)') {
                        $offset = $Matches[1]
                        Write-Host "    $line" -ForegroundColor White
                    }
                    if ($line -match '(public|private|protected|internal)\s+\S+\s+(\w+)\s*;' -and $line -notmatch 'class ') {
                        $fieldName = $Matches[2]
                        Write-Host "    FIELD: $fieldName" -ForegroundColor Magenta
                    }
                }
            }
        }
    }

    if (-not $found) {
        Get-ChildItem -Path $Path -Recurse -Filter "*.txt" -ErrorAction SilentlyContinue | ForEach-Object {
            $content = Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue
            if ($content -and $content -match $cls) {
                Write-Host "  Found in: $($_.FullName)" -ForegroundColor Green
                $lines = Get-Content $_.FullName
                foreach ($line in $lines) {
                    if ($line -match $cls -or $line -match '0x[0-9A-Fa-f]+') {
                        $trimmed = $line.Trim()
                        if ($trimmed.Length -gt 0 -and $trimmed.Length -lt 200) {
                            Write-Host "    $trimmed"
                        }
                    }
                }
            }
        }
    }

    if (-not $found) {
        Write-Host "  NOT FOUND" -ForegroundColor Red
    }
    Write-Host ""
}

Write-Host "`n=== what to put in offsets.cfg ===" -ForegroundColor Cyan
Write-Host @"

Look at the output above and fill these in:

battleModelTypeInfo = 0x0          <-- set to 0, auto-detect works
bmEntities         = 0x??         <-- BattleModel -> BattleEntitiesModel field offset
bePlayer           = 0x??         <-- BattleEntitiesModel -> Player field offset
beModels           = 0x??         <-- BattleEntitiesModel -> Models (dictionary/list) offset
localServerModel   = 0x??         <-- PlayerEntityModel -> ServerModel offset
entityServerModel  = 0x??         <-- BattleCharacterEntityModel -> ServerModel offset
cmTransform        = 0x??         <-- BattleCharacterModel -> Transform offset
cmIsDead           = 0x??         <-- BattleCharacterModel -> IsDead offset
cmRole             = 0x??         <-- BattleCharacterModel -> Role offset
cmHealth           = 0x??         <-- BattleCharacterModel -> Health offset

The offset is the hex number from [FieldOffset(Offset = "0x...")] PLUS 0x10
(il2cpp object header is 0x10 bytes: klass ptr + monitor ptr)

Example: if Cpp2IL says Offset = "0x18", the real offset is 0x18 + 0x10 = 0x28
WAIT: Cpp2IL usually already includes the 0x10 header. Check the first field.
If the first field starts at 0x10, offsets are already correct (header included).
If the first field starts at 0x00, add 0x10 to every offset.

"@

Write-Host "Done. Paste the values into offsets.cfg next to your exe." -ForegroundColor Green
