$ErrorActionPreference = "Stop"

# ============================================================
# BRIO DUAL SPOUT V17.1 - AUTOINSTALLER 17.1.2
# ============================================================
# Checks/installs:
#   - Logitech BRIO physical MI_00 presence
#   - SensorCameraMode=2 (with registry backup)
#   - Visual Studio 2022 Build Tools C++ workload + Windows SDK
#   - Microsoft Visual C++ x64 Redistributable
#   - Latest official Spout2 SDK binaries
# Then:
#   - Compiles V17.1
#   - Creates Desktop + Start Menu shortcuts
#   - Launches the application
#
# Does NOT:
#   - flash firmware
#   - replace Logitech firmware
#   - install unofficial camera drivers
# ============================================================

function Step([string]$m) {
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host $m -ForegroundColor Yellow
    Write-Host "============================================================" -ForegroundColor Cyan
}

function Is-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Download-File([string]$Url, [string]$OutFile) {
    Write-Host "Descargando: $Url" -ForegroundColor DarkGray
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $OutFile
}

function Get-VsInstall {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

    if(-not (Test-Path $vswhere)) {
        return $null
    }

    $p = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null

    if(-not $p) {
        return $null
    }

    $p = $p.Trim()
    $dev = Join-Path $p "Common7\Tools\VsDevCmd.bat"

    if(-not (Test-Path $dev)) {
        return $null
    }

    return $p
}

function Has-WindowsSdk {
    $root = "${env:ProgramFiles(x86)}\Windows Kits\10\Lib"

    if(-not (Test-Path $root)) {
        return $false
    }

    $lib = Get-ChildItem $root -Filter windowsapp.lib -File -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1

    return [bool]$lib
}

function Ensure-BuildTools([string]$Cache) {
    $vs = Get-VsInstall
    $sdk = Has-WindowsSdk

    if($vs -and $sdk) {
        Write-Host "Visual Studio C++: OK" -ForegroundColor Green
        Write-Host "Windows SDK:       OK" -ForegroundColor Green
        return $vs
    }

    Step "Instalando Microsoft Visual Studio Build Tools C++"

    Write-Host "No esta completo el entorno C++/Windows SDK." -ForegroundColor Yellow
    Write-Host "Se instalara desde el bootstrapper oficial de Microsoft." -ForegroundColor Yellow
    Write-Host "Esto puede descargar varios GB la primera vez." -ForegroundColor Yellow

    $bootstrap = Join-Path $Cache "vs_BuildTools.exe"

    if(-not (Test-Path $bootstrap)) {
        Download-File "https://aka.ms/vs/17/release/vs_BuildTools.exe" $bootstrap
    }

    $args = @(
        "--quiet",
        "--wait",
        "--norestart",
        "--nocache",
        "--add", "Microsoft.VisualStudio.Workload.VCTools",
        "--includeRecommended"
    )

    $p = Start-Process -FilePath $bootstrap -ArgumentList $args -Wait -PassThru

    if($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {
        throw "Visual Studio Build Tools fallo con codigo $($p.ExitCode)."
    }

    $vs = Get-VsInstall
    $sdk = Has-WindowsSdk

    if(-not $vs -or -not $sdk) {
        throw "Build Tools termino, pero C++/Windows SDK aun no estan completos."
    }

    if($p.ExitCode -eq 3010) {
        Write-Host "Microsoft indico que Windows recomienda reiniciar, pero intentaremos continuar." -ForegroundColor Yellow
    }

    Write-Host "Visual Studio C++ + Windows SDK: INSTALADOS" -ForegroundColor Green
    return $vs
}

function Test-VcRuntime {
    $paths = @(
        "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"
    )

    foreach($p in $paths) {
        try {
            $x = Get-ItemProperty -Path $p -ErrorAction Stop
            if($x.Installed -eq 1) {
                return $true
            }
        } catch {}
    }

    return $false
}

function Ensure-VcRuntime([string]$Cache) {
    if(Test-VcRuntime) {
        Write-Host "Visual C++ x64 Runtime: OK" -ForegroundColor Green
        return
    }

    Step "Instalando Microsoft Visual C++ x64 Runtime"

    $redist = Join-Path $Cache "vc_redist.x64.exe"

    if(-not (Test-Path $redist)) {
        Download-File "https://aka.ms/vc14/vc_redist.x64.exe" $redist
    }

    $p = Start-Process -FilePath $redist `
        -ArgumentList @("/install","/quiet","/norestart") `
        -Wait -PassThru

    if($p.ExitCode -notin @(0,1638,3010)) {
        throw "VC++ Redistributable fallo con codigo $($p.ExitCode)."
    }

    Write-Host "Visual C++ x64 Runtime: INSTALADO" -ForegroundColor Green
}

function Get-BrioMi00 {
    $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object {
            $_.InstanceId -match '^USB\\VID_046D&PID_085E&MI_00\\' -and
            $_.Status -eq 'OK'
        } |
        Select-Object -First 1

    if(-not $d) {
        $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object {
                $_.InstanceId -match '^USB\\VID_046D&PID_085E&MI_00\\'
            } |
            Select-Object -First 1
    }

    return $d
}

function Wait-CameraFree {
    $patterns = @(
        'BRIO_DUAL_SPOUT',
        'obs64',
        'LogiTune',
        'WindowsCamera',
        'Camera',
        'TouchDesigner'
    )

    while($true) {
        $busy = Get-Process -ErrorAction SilentlyContinue |
            Where-Object {
                $name = $_.ProcessName
                ($patterns | Where-Object { $name -like "*$_*" }).Count -gt 0
            }

        if(-not $busy) {
            return
        }

        Write-Host ""
        Write-Host "Hay aplicaciones que pueden tener tomada la BRIO:" -ForegroundColor Yellow
        $busy | Sort-Object ProcessName -Unique |
            Format-Table ProcessName,Id -AutoSize

        Write-Host "Cerralas para que el instalador pueda reiniciar/verificar la camara." -ForegroundColor Yellow
        Read-Host "ENTER para volver a comprobar"
    }
}

function Configure-SensorCamera(
    [object]$Device,
    [string]$BackupDir,
    [string]$StatePath
) {
    $instance = $Device.InstanceId
    $native = "HKLM\SYSTEM\CurrentControlSet\Enum\$instance\Device Parameters"
    $psPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\$instance\Device Parameters"

    if(-not (Test-Path $psPath)) {
        throw "No existe Device Parameters para $instance"
    }

    $existingState = $null

    if(Test-Path $StatePath) {
        try {
            $existingState = Get-Content $StatePath -Raw | ConvertFrom-Json
        } catch {}
    }

    $props = Get-ItemProperty -Path $psPath
    $had = $props.PSObject.Properties.Name -contains "SensorCameraMode"
    $old = $null

    if($had) {
        $old = [int]$props.SensorCameraMode
    }

    # Preserve the state from before the FIRST installer run.
    if(-not $existingState) {
        $backup = Join-Path $BackupDir "BRIO_Device_Parameters_before_install.reg"
        & reg.exe export $native $backup /y | Out-Null

        $state = [ordered]@{
            version = "17.1.2"
            brio_instance_id = $instance
            sensor_camera_mode_previous_exists = [bool]$had
            sensor_camera_mode_previous_value = $old
            registry_backup = $backup
            installed_at = (Get-Date).ToString("o")
        }

        $state | ConvertTo-Json -Depth 4 |
            Set-Content -Path $StatePath -Encoding UTF8
    }

    if($had -and $old -eq 2) {
        Write-Host "SensorCameraMode=2: OK" -ForegroundColor Green
        return $false
    }

    Step "Configurando BRIO como Sensor Camera"

    Wait-CameraFree

    New-ItemProperty `
        -Path $psPath `
        -Name SensorCameraMode `
        -PropertyType DWord `
        -Value 2 `
        -Force | Out-Null

    $verify = (Get-ItemProperty -Path $psPath -Name SensorCameraMode).SensorCameraMode

    if($verify -ne 2) {
        throw "No pude fijar SensorCameraMode=2."
    }

    Write-Host "SensorCameraMode=2: APLICADO" -ForegroundColor Green

    Write-Host "Reiniciando solo la interfaz MI_00 de la BRIO..." -ForegroundColor Cyan

    & pnputil.exe /restart-device "$instance" | Out-Host

    Start-Sleep -Seconds 3

    return $true
}

function Ensure-Spout(
    [string]$Deps,
    [string]$Cache,
    [string]$AppDir
) {
    Step "Comprobando Spout2 oficial"

    $release = Invoke-RestMethod `
        -Uri "https://api.github.com/repos/leadedge/Spout2/releases/latest" `
        -Headers @{ "User-Agent" = "BRIO-DUAL-SPOUT-V17-1-AUTOINSTALLER" }

    $asset = $release.assets |
        Where-Object { $_.name -like "Spout-SDK-binaries*.zip" } |
        Select-Object -First 1

    if(-not $asset) {
        throw "La release actual de Spout2 no contiene Spout-SDK-binaries*.zip."
    }

    Write-Host "Spout2 release: $($release.tag_name)" -ForegroundColor Green

    $zip = Join-Path $Cache $asset.name
    $extract = Join-Path $Deps "spout_release"

    if(-not (Test-Path $zip)) {
        Download-File $asset.browser_download_url $zip
    }

    if(Test-Path $extract) {
        Remove-Item $extract -Recurse -Force
    }

    Expand-Archive $zip -DestinationPath $extract -Force

    $header = Get-ChildItem $extract -Filter SpoutLibrary.h -Recurse -File -ErrorAction SilentlyContinue |
        Select-Object -First 1

    if(-not $header) {
        $headerPath = Join-Path $Deps "SpoutLibrary.h"
        $tag = $release.tag_name

        try {
            Download-File `
                "https://raw.githubusercontent.com/leadedge/Spout2/$tag/SPOUTSDK/SpoutLibrary/SpoutLibrary.h" `
                $headerPath
        }
        catch {
            Download-File `
                "https://raw.githubusercontent.com/leadedge/Spout2/master/SPOUTSDK/SpoutLibrary/SpoutLibrary.h" `
                $headerPath
        }
    }
    else {
        $headerPath = $header.FullName
    }

    $dlls = Get-ChildItem $extract -Filter SpoutLibrary.dll -Recurse -File -ErrorAction SilentlyContinue

    if(-not $dlls) {
        throw "No se encontro SpoutLibrary.dll en el SDK."
    }

    $dll = $dlls |
        Sort-Object @{
            Expression = {
                if($_.FullName -match 'x64|64') { 0 }
                elseif($_.FullName -match 'Win32|x86') { 2 }
                else { 1 }
            }
        } |
        Select-Object -First 1

    Copy-Item $dll.FullName (Join-Path $AppDir "SpoutLibrary.dll") -Force

    return [pscustomobject]@{
        Version = $release.tag_name
        Header = $headerPath
        IncludeDir = Split-Path -Parent $headerPath
        Dll = $dll.FullName
    }
}

function Build-App(
    [string]$VsPath,
    [string]$SourceDir,
    [string]$AppDir,
    [string]$IncludeDir
) {
    Step "Compilando BRIO Dual Spout V17.1"

    $devcmd = Join-Path $VsPath "Common7\Tools\VsDevCmd.bat"
    $cpp = Join-Path $SourceDir "BrioDualSpoutV17_1.cpp"
    $exe = Join-Path $AppDir "BRIO_DUAL_SPOUT_V17_1.exe"

    if(-not (Test-Path $cpp)) {
        throw "No encuentro el codigo fuente: $cpp"
    }

    if(Test-Path $exe) {
        Remove-Item $exe -Force
    }

    $cmd = 'call "{0}" -arch=x64 -host_arch=x64 && cd /d "{1}" && cl /O2 /nologo /EHsc /std:c++17 /await /DUNICODE /D_UNICODE /I"{2}" "BrioDualSpoutV17_1.cpp" /Fe:"{3}" /link /SUBSYSTEM:WINDOWS windowsapp.lib user32.lib gdi32.lib shell32.lib opengl32.lib' -f `
        $devcmd,$SourceDir,$IncludeDir,$exe

    cmd.exe /d /s /c $cmd | Out-Host

    if($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        throw "Fallo la compilacion de V17.1."
    }

    Write-Host "Aplicacion compilada: $exe" -ForegroundColor Green
    return [string]$exe
}

function Write-LauncherCmd(
    [string]$Path,
    [string]$Target,
    [string]$WorkingDir
) {
    $parent = Split-Path -Parent $Path

    if($parent -and -not (Test-Path $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    $content = @"
@echo off
cd /d "$WorkingDir"
start "" "$Target"
"@

    $content | Set-Content -Path $Path -Encoding ASCII
}

if(-not (Is-Admin)) {
    throw "Este instalador debe ejecutarse como administrador. Usa SETUP_AND_RUN.bat."
}

$SourcePackage = Split-Path -Parent $MyInvocation.MyCommand.Path
$InstallRoot = Join-Path $env:LOCALAPPDATA "BRIO_DUAL_SPOUT\V17_1"
$SourceDir = Join-Path $InstallRoot "src"
$AppDir = Join-Path $InstallRoot "app"
$Deps = Join-Path $InstallRoot "deps"
$Cache = Join-Path $InstallRoot "cache"
$Backup = Join-Path $InstallRoot "backup"
$Logs = Join-Path $InstallRoot "logs"
$State = Join-Path $InstallRoot "install_state.json"

New-Item -ItemType Directory -Force `
    -Path $InstallRoot,$SourceDir,$AppDir,$Deps,$Cache,$Backup,$Logs |
    Out-Null

$log = Join-Path $Logs ("install_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".txt")
Start-Transcript -Path $log -Force | Out-Null

try {
    Step "BRIO Dual Spout V17.1 - instalacion automatica"

    Write-Host "Destino: $InstallRoot" -ForegroundColor Cyan
    Write-Host "Log:     $log" -ForegroundColor DarkGray

    if(-not [Environment]::Is64BitOperatingSystem) {
        throw "Esta version requiere Windows x64."
    }

    Step "1/8 - Comprobando Logitech BRIO"

    $brio = Get-BrioMi00

    if(-not $brio) {
        throw "No encuentro la BRIO fisica VID_046D PID_085E MI_00. Conectala directamente por USB y vuelve a ejecutar."
    }

    $brio | Format-List Status,Class,FriendlyName,InstanceId

    Step "2/8 - SensorCameraMode"

    $changedSensor = Configure-SensorCamera `
        -Device $brio `
        -BackupDir $Backup `
        -StatePath $State

    Step "3/8 - Microsoft C++ Build Tools + Windows SDK"

    $vs = Ensure-BuildTools $Cache

    Step "4/8 - Microsoft Visual C++ Runtime"

    Ensure-VcRuntime $Cache

    Step "5/8 - Copiando V17.1"

    Copy-Item `
        (Join-Path $SourcePackage "BrioDualSpoutV17_1.cpp") `
        (Join-Path $SourceDir "BrioDualSpoutV17_1.cpp") `
        -Force

    if(Test-Path (Join-Path $SourcePackage "README.txt")) {
        Copy-Item `
            (Join-Path $SourcePackage "README.txt") `
            (Join-Path $InstallRoot "README.txt") `
            -Force
    }

    Step "6/8 - Spout2"

    $spout = Ensure-Spout `
        -Deps $Deps `
        -Cache $Cache `
        -AppDir $AppDir

    Step "7/8 - Build + accesos directos"

    $exe = Build-App `
        -VsPath $vs `
        -SourceDir $SourceDir `
        -AppDir $AppDir `
        -IncludeDir $spout.IncludeDir

    # Defensive guard: FilePath must always receive exactly one string.
    if($exe -is [System.Array]) {
        $exe = [string]($exe | Select-Object -Last 1)
    } else {
        $exe = [string]$exe
    }

    $launcher = Join-Path $InstallRoot "RUN_BRIO_DUAL_SPOUT.cmd"

    @"
@echo off
cd /d "$AppDir"
start "" "$exe"
"@ | Set-Content -Path $launcher -Encoding ASCII

    # Copy the uninstaller into the installed folder.
    Copy-Item `
        (Join-Path $SourcePackage "UNINSTALL.ps1") `
        (Join-Path $InstallRoot "UNINSTALL.ps1") `
        -Force

    Copy-Item `
        (Join-Path $SourcePackage "UNINSTALL.bat") `
        (Join-Path $InstallRoot "UNINSTALL.bat") `
        -Force

    # Avoid COM shortcut creation entirely. On some Windows 11 systems
    # WScript.Shell.CreateShortcut can throw E_INVALIDARG ("El valor no esta
    # dentro del intervalo esperado") even after a successful build.
    # Plain CMD launchers are robust and need no COM.
    $desktop = [Environment]::GetFolderPath("Desktop")

    if(-not (Test-Path $desktop)) {
        New-Item -ItemType Directory -Force -Path $desktop | Out-Null
    }

    $desktopLauncher = Join-Path $desktop "BRIO Dual Spout V17.1.cmd"

    Write-LauncherCmd `
        -Path $desktopLauncher `
        -Target $exe `
        -WorkingDir $AppDir

    $startFolder = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\BRIO Dual Spout"
    New-Item -ItemType Directory -Force -Path $startFolder | Out-Null

    Write-LauncherCmd `
        -Path (Join-Path $startFolder "BRIO Dual Spout V17.1.cmd") `
        -Target $exe `
        -WorkingDir $AppDir

    # Add current install metadata without overwriting the original registry baseline.
    $stateObj = Get-Content $State -Raw | ConvertFrom-Json
    $stateObj | Add-Member -NotePropertyName spout_version -NotePropertyValue $spout.Version -Force
    $stateObj | Add-Member -NotePropertyName install_root -NotePropertyValue $InstallRoot -Force
    $stateObj | Add-Member -NotePropertyName executable -NotePropertyValue $exe -Force
    $stateObj | Add-Member -NotePropertyName last_install_completed -NotePropertyValue (Get-Date).ToString("o") -Force
    $stateObj | ConvertTo-Json -Depth 6 | Set-Content $State -Encoding UTF8

    Step "8/8 - Lanzando V17.1"

    Write-Host "DEPENDENCIAS: OK" -ForegroundColor Green
    Write-Host "SensorCameraMode: 2" -ForegroundColor Green
    Write-Host "Spout2: $($spout.Version)" -ForegroundColor Green
    Write-Host "Aplicacion: $exe" -ForegroundColor Green
    Write-Host ""
    Write-Host "Senders esperados:" -ForegroundColor Cyan
    Write-Host "  BRIO_RGB" -ForegroundColor Cyan
    Write-Host "  BRIO_IR" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Perfil actual: RGB 800x600 YUY2 @30 + IR 340x340 L8 @30" -ForegroundColor Cyan
    Write-Host "Orden: IR -> 100 ms -> RGB" -ForegroundColor Cyan

    Wait-CameraFree

    $proc = Start-Process -FilePath $exe -WorkingDirectory $AppDir -PassThru

    Start-Sleep -Seconds 4

    if($proc.HasExited) {
        Write-Host "La aplicacion salio durante el arranque. Revisa el mensaje mostrado por V17.1 y el log." -ForegroundColor Yellow
    }
    else {
        Write-Host "V17.1 esta ejecutandose." -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "INSTALACION COMPLETA" -ForegroundColor Green
    Write-Host "Lanzador creado en Escritorio y Menu Inicio." -ForegroundColor Green
}
catch {
    Write-Host ""
    Write-Host "ERROR DE INSTALACION:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host ""
    Write-Host "Log: $log" -ForegroundColor Yellow

    Read-Host "ENTER para cerrar"
    exit 1
}
finally {
    try { Stop-Transcript | Out-Null } catch {}
}

Read-Host "ENTER para cerrar el instalador"
