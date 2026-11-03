# Read-only inventory by default. Invoke with -Install only after building,
# testing, packaging and publishing. No console UI control or boot changes.
[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$WhatIf,
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')][string]$ExpectedVersion = '0.2.2',
    [ValidateRange(5,30)][int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$channel = 'https://raw.githubusercontent.com/ANE-JV/ninmagic-saves/main/update.txt'
$validationPath = Join-Path $projectRoot 'dist/validation.json'
$nroPath = Join-Path $projectRoot 'dist/NinMagic-Saves.nro'
$publicationRoot = Join-Path $projectRoot 'dist/publication'
$packagePath = Join-Path $publicationRoot "versions/v$ExpectedVersion/NinMagic-Saves-$ExpectedVersion.zip"

function Assert-RegularFile([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "No es un archivo local normal: $Path"
    }
}
function Get-Sha([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-BytesSha([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

# Validate the exact packaged build before connecting to the MTP namespace.
foreach ($path in @($validationPath,$nroPath,$packagePath)) { Assert-RegularFile $path }
$validation = Get-Content -LiteralPath $validationPath -Raw | ConvertFrom-Json
if ($validation.title -cne 'NinMagic Saves' -or $validation.version -cne $ExpectedVersion -or
    $validation.manifest -cne $channel -or $validation.package -cne [IO.Path]::GetFileName($packagePath)) {
    throw 'validation.json no identifica esta versión, ZIP y canal oficial. Ejecuta build, tests y package primero.'
}
$expectedNroHash = Get-Sha $nroPath
if ($expectedNroHash -cne $validation.sha256 -or (Get-Item -LiteralPath $nroPath).Length -ne [long]$validation.bytes -or
    (Get-Sha $packagePath) -cne $validation.package_sha256) {
    throw 'El NRO o ZIP no coincide con validation.json. No se transfiere nada.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
$payload = [Collections.Generic.List[object]]::new()
try {
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($entry in $archive.Entries) {
        $name = $entry.FullName
        if (-not $seen.Add($name)) { throw 'El ZIP tiene nombres repetidos.' }
        $allowed = $name -cin @('switch/NinMagicSaves/NinMagic-Saves.nro','switch/NinMagicSaves/LEEME.txt','config/NinMagicSaves/update-url.txt') -or
                   $name -cmatch '^switch/NinMagicSaves/licencias/[A-Za-z0-9_.-]+\.txt$'
        if (-not $allowed -or $name.Contains('..') -or $entry.Length -gt 16777216 -or $entry.Length -le 0) {
            throw "Entrada no admitida en el paquete: $name"
        }
        $stream = $entry.Open()
        $memory = [IO.MemoryStream]::new()
        try { $stream.CopyTo($memory); $bytes = $memory.ToArray() }
        finally { $stream.Dispose(); $memory.Dispose() }
        if ($bytes.LongLength -ne $entry.Length) { throw "Entrada incompleta: $name" }
        $payload.Add([pscustomobject]@{Relative=$name;Bytes=$bytes;Size=[long]$bytes.LongLength;Sha256=(Get-BytesSha $bytes);Source=$null})
    }
} finally { $archive.Dispose() }
$actualNames = @($payload.Relative | Sort-Object -CaseSensitive)
$validatedNames = @($validation.entries | Sort-Object -CaseSensitive)
if (($actualNames -join "`n") -cne ($validatedNames -join "`n")) { throw 'El inventario ZIP no coincide con validation.json.' }
foreach ($required in @('switch/NinMagicSaves/NinMagic-Saves.nro','switch/NinMagicSaves/LEEME.txt','config/NinMagicSaves/update-url.txt')) {
    if (@($payload | Where-Object Relative -CEQ $required).Count -ne 1) { throw "Falta la entrada obligatoria $required" }
}
$packagedNro = @($payload | Where-Object Relative -CEQ 'switch/NinMagicSaves/NinMagic-Saves.nro')[0]
if ($packagedNro.Sha256 -cne $expectedNroHash) { throw 'El NRO del ZIP difiere del NRO validado.' }
$packagedChannel = @($payload | Where-Object Relative -CEQ 'config/NinMagicSaves/update-url.txt')[0]
if ([Text.Encoding]::ASCII.GetString($packagedChannel.Bytes) -cne ($channel + "`n")) {
    throw 'El canal del ZIP no coincide exactamente con el canal verificado.'
}

$shell = New-Object -ComObject Shell.Application
$computer = $shell.NameSpace(17)
if ($null -eq $computer) { throw 'No se puede enumerar Este equipo.' }
$devices = @($computer.Items() | Where-Object { $_.IsFolder -and $_.Name -in @('Switch','Nintendo Switch') })
if ($devices.Count -ne 1) { throw 'Debe aparecer exactamente una Switch. Conéctala y abre DBI > Run MTP responder.' }
$sdItems = @($devices[0].GetFolder.Items() | Where-Object { $_.IsFolder -and $_.Name -ceq '1: SD Card' })
if ($sdItems.Count -ne 1) { throw 'No se encuentra una única microSD 1: SD Card de DBI.' }
$sd = $sdItems[0].GetFolder

function Find-Folder($Parent,[string]$Name) {
    $items = @($Parent.Items() | Where-Object { $_.Name -ieq $Name })
    if ($items.Count -gt 1 -or ($items.Count -eq 1 -and -not $items[0].IsFolder)) {
        throw "Destino de carpeta ambiguo: $Name"
    }
    if ($items.Count -eq 1) { return $items[0].GetFolder }
    return $null
}
function Find-Leaf($Parent,[string]$Name) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($Name)
    $extension = [IO.Path]::GetExtension($Name)
    $leafMatches = @($Parent.Items() | Where-Object { $_.Name -ieq $Name -or $_.Name -ieq $stem })
    if ($leafMatches.Count -gt 1 -or ($leafMatches.Count -eq 1 -and $leafMatches[0].IsFolder)) {
        throw "Nombre de archivo ambiguo o usado por carpeta: $Name"
    }
    if ($leafMatches.Count -eq 0) { return $null }
    $knownExtension = $leafMatches[0].ExtendedProperty('System.FileExtension')
    if ($knownExtension -and [string]$knownExtension -ine $extension) {
        throw "La extensión real del destino no coincide: $Name"
    }
    return $leafMatches[0]
}
function Remote-Size($Item) {
    $raw = $Item.ExtendedProperty('System.Size')
    if ($null -eq $raw -or [string]::IsNullOrWhiteSpace([string]$raw)) { throw 'MTP no informa un tamaño fiable del archivo.' }
    [long]$size = 0
    if (-not [long]::TryParse([string]$raw,[ref]$size) -or $size -lt 0 -or $size -gt 16777216) {
        throw 'El tamaño del archivo MTP no es válido o supera el límite de esta aplicación.'
    }
    return $size
}
function Wait-Folder($Parent,[string]$Name) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $found = Find-Folder $Parent $Name
        if ($null -ne $found) { return $found }
        Start-Sleep -Milliseconds 250
    } while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds)
    throw "MTP no ha creado la carpeta $Name dentro del plazo. Se detiene sin borrar nada."
}
function Ensure-Folder($Parent,[string]$Name) {
    if ($Name -cnotin @('switch','config','NinMagicSaves','licencias')) { throw 'Carpeta fuera del alcance autorizado.' }
    $found = Find-Folder $Parent $Name
    if ($null -ne $found) { return $found }
    [void]$Parent.NewFolder($Name)
    Wait-Folder $Parent $Name
}
function Wait-Leaf($Parent,[string]$Name,[long]$Size) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $stable = 0
    do {
        $found = Find-Leaf $Parent $Name
        if ($null -ne $found -and (Remote-Size $found) -eq $Size) {
            $stable++
            if ($stable -ge 2) { return $found }
        } else { $stable = 0 }
        Start-Sleep -Milliseconds 250
    } while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds)
    throw "No se confirmó la copia MTP de $Name dentro del plazo. Se conservan los archivos anteriores."
}

# Preview visits only the two application folders. It does not inspect saves,
# NAND partitions, credentials or other applications, and creates no local files.
$preview = [Collections.Generic.List[object]]::new()
$switchFolder = Find-Folder $sd 'switch'
$configFolder = Find-Folder $sd 'config'
$appFolder = if ($null -ne $switchFolder) { Find-Folder $switchFolder 'NinMagicSaves' } else { $null }
$cfgFolder = if ($null -ne $configFolder) { Find-Folder $configFolder 'NinMagicSaves' } else { $null }
$licenseFolder = if ($null -ne $appFolder) { Find-Folder $appFolder 'licencias' } else { $null }
foreach ($item in $payload) {
    $parent = if ($item.Relative.StartsWith('config/')) { $cfgFolder }
              elseif ($item.Relative.Contains('/licencias/')) { $licenseFolder } else { $appFolder }
    $remote = if ($null -ne $parent) { Find-Leaf $parent ([IO.Path]::GetFileName($item.Relative)) } else { $null }
    $preview.Add([pscustomobject]@{Destination=('sdmc:/'+$item.Relative);Exists=($null -ne $remote);Bytes=$item.Size;Sha256=$item.Sha256})
}
if (-not $Install -or $WhatIf) {
    [pscustomobject]@{Mode='Inventory only';Version=$ExpectedVersion;Validated=$true;InstallRequested=[bool]$Install;WhatIf=[bool]$WhatIf;Files=$preview.ToArray();Instruction='Para copiar estos archivos, ejecuta de nuevo con -Install. No se modificó la consola.'} | ConvertTo-Json -Depth 5
    return
}

# All receipts and old executables stay in a git-ignored, dedicated local area.
$ignore = Get-Content -LiteralPath (Join-Path $projectRoot '.gitignore') -Raw
if ($ignore -notmatch '(?m)^private/\r?$') { throw 'private/ debe estar excluido del repositorio antes de respaldar archivos.' }
$privateRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'private'))
if (-not $privateRoot.StartsWith($projectRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Ruta privada fuera del proyecto.' }
if (Test-Path -LiteralPath $privateRoot) {
    $privateItem = Get-Item -LiteralPath $privateRoot -Force
    if (-not $privateItem.PSIsContainer -or ($privateItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'private debe ser una carpeta local normal.' }
} else { $null = New-Item -ItemType Directory -Path $privateRoot }
$receiptRoot = Join-Path $privateRoot ('mtp-'+[guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $receiptRoot
$receipt = [ordered]@{Version=$ExpectedVersion;StartedUtc=[DateTime]::UtcNow.ToString('o');Completed=$false;Files=[Collections.Generic.List[object]]::new();Error=$null}
function Save-Receipt {
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $receiptRoot 'receipt.json') -Encoding utf8
}
function Read-Back($Remote,[string]$LocalDir,[long]$ExpectedSize,[string]$ExpectedHash='') {
    $null = New-Item -ItemType Directory -Path $LocalDir
    $namespace = $shell.NameSpace($LocalDir)
    if ($null -eq $namespace) { throw 'Shell no puede abrir la carpeta privada de verificación.' }
    $namespace.CopyHere($Remote,1556)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $lastHash = ''
    $stable = 0
    do {
        $localFiles = @(Get-ChildItem -LiteralPath $LocalDir -Force -File)
        if ($localFiles.Count -gt 1) { throw 'La lectura de vuelta produjo más de un archivo; se detiene.' }
        if ($localFiles.Count -eq 1 -and $localFiles[0].Length -eq $ExpectedSize) {
            $opened = $null
            try {
                # Exclusive access rules out hashing a still-open copy writer.
                $opened = [IO.File]::Open($localFiles[0].FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
                $sha = [Security.Cryptography.SHA256]::Create()
                try { $hash = ([BitConverter]::ToString($sha.ComputeHash($opened))).Replace('-','').ToLowerInvariant() }
                finally { $sha.Dispose() }
                if (($ExpectedHash -eq '' -or $hash -ceq $ExpectedHash) -and $hash -ceq $lastHash) { $stable++ }
                else { $stable = 0 }
                $lastHash = $hash
                if ($stable -ge 1) { return [pscustomobject]@{Path=$localFiles[0].FullName;Sha256=$hash;Bytes=$ExpectedSize} }
            } catch [IO.IOException] { $stable = 0 }
            finally { if ($null -ne $opened) { $opened.Dispose() } }
        }
        Start-Sleep -Milliseconds 250
    } while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds)
    throw 'No se pudo verificar la lectura de vuelta MTP. No se borra ni reemplaza ningún respaldo.'
}

try {
    $staging = Join-Path $receiptRoot 'incoming'
    $null = New-Item -ItemType Directory -Path $staging
    foreach ($item in $payload) {
        $relativeLocal = $item.Relative.Replace('/',[IO.Path]::DirectorySeparatorChar)
        $item.Source = Join-Path $staging $relativeLocal
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($item.Source)) -Force
        [IO.File]::WriteAllBytes($item.Source,$item.Bytes)
        if ((Get-Sha $item.Source) -cne $item.Sha256) { throw 'La copia local de preparación no coincide con el paquete.' }
    }
    Save-Receipt
    $switchFolder = Ensure-Folder $sd 'switch'
    $configFolder = Ensure-Folder $sd 'config'
    $appFolder = Ensure-Folder $switchFolder 'NinMagicSaves'
    $cfgFolder = Ensure-Folder $configFolder 'NinMagicSaves'
    $licenseFolder = Ensure-Folder $appFolder 'licencias'
    # Serial transfers: never issue simultaneous CopyHere to one MTP device.
    foreach ($item in ($payload | Sort-Object Relative)) {
        $name = [IO.Path]::GetFileName($item.Relative)
        $parent = if ($item.Relative.StartsWith('config/')) { $cfgFolder }
                  elseif ($item.Relative.Contains('/licencias/')) { $licenseFolder } else { $appFolder }
        $record = [ordered]@{Destination=('sdmc:/'+$item.Relative);ExpectedSha256=$item.Sha256;Bytes=$item.Size;Previous=$null;BackupName=$null;Installed=$false;Verified=$false;Readback=$null}
        $receipt.Files.Add($record)
        Save-Receipt
        $old = Find-Leaf $parent $name
        if ($null -ne $old) {
            $oldSize = Remote-Size $old
            $backupId = [guid]::NewGuid().ToString('N')
            $first = Read-Back $old (Join-Path $receiptRoot ("before-$backupId")) $oldSize
            $second = Read-Back $old (Join-Path $receiptRoot ("before-check-$backupId")) $oldSize $first.Sha256
            if ($first.Sha256 -cne $second.Sha256) { throw 'El archivo anterior cambió durante el respaldo.' }
            $record.Previous = [ordered]@{Path=$first.Path;Sha256=$first.Sha256;Bytes=$oldSize;SecondReadback=$second.Path}
            Save-Receipt
            if ($first.Sha256 -ceq $item.Sha256 -and $oldSize -eq $item.Size) {
                $record.Installed=$true; $record.Verified=$true; $record.Readback=$second.Path
                Save-Receipt
                continue
            }
            $backupStem = '.'+[IO.Path]::GetFileNameWithoutExtension($name)+'-previous-'+$backupId
            $backupName = $backupStem+[IO.Path]::GetExtension($name)
            if ($null -ne (Find-Leaf $parent $backupName)) { throw 'Colisión inesperada de nombre de respaldo; no se modifica.' }
            # Rename only this same FolderItem inside its own existing folder.
            # No MoveHere, Delete, filesystem recursion or NAND/savedata access.
            $renameDisplay = if ($old.Name.EndsWith([IO.Path]::GetExtension($name),[StringComparison]::OrdinalIgnoreCase)) { $backupName } else { $backupStem }
            $record.BackupName=$backupName
            Save-Receipt
            $old.Name=$renameDisplay
            $renamed = Wait-Leaf $parent $backupName $oldSize
            if ($null -ne (Find-Leaf $parent $name)) { throw 'El nombre original sigue ocupado tras renombrarlo. No se sobrescribe.' }
            $renamedCheck = Read-Back $renamed (Join-Path $receiptRoot ("renamed-check-$backupId")) $oldSize $first.Sha256
            $record.Previous.RenamedReadback=$renamedCheck.Path
            Save-Receipt
        }
        $sourceFolder = $shell.NameSpace([IO.Path]::GetDirectoryName($item.Source))
        $sourceItem = $sourceFolder.ParseName([IO.Path]::GetFileName($item.Source))
        if ($null -eq $sourceItem) { throw 'Shell no puede abrir el archivo preparado.' }
        if ($null -ne (Find-Leaf $parent $name)) { throw 'El destino apareció antes de copiar; no se sobrescribe.' }
        $parent.CopyHere($sourceItem,1556)
        $copied = Wait-Leaf $parent $name $item.Size
        $record.Installed=$true
        Save-Receipt
        $readback = Read-Back $copied (Join-Path $receiptRoot ('installed-check-'+[guid]::NewGuid().ToString('N'))) $item.Size $item.Sha256
        $record.Verified=$true; $record.Readback=$readback.Path
        Save-Receipt
    }
    $receipt.Completed=$true
    $receipt.CompletedUtc=[DateTime]::UtcNow.ToString('o')
    Save-Receipt
    [pscustomobject]@{Installed='sdmc:/switch/NinMagicSaves/NinMagic-Saves.nro';Version=$ExpectedVersion;Sha256=$expectedNroHash;Verified=$true;Receipt=(Join-Path $receiptRoot 'receipt.json');BootFilesChanged=$false;SavesAccessed=$false} | ConvertTo-Json
} catch {
    $receipt.Error=$_.Exception.Message
    Save-Receipt
    throw ($_.Exception.Message+" Respaldos y registro conservados en: $receiptRoot. No se ha borrado ningún archivo.")
}
