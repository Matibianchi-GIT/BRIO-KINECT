$ErrorActionPreference = "Stop"

function Is-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if(-not (Is-Admin)) {
    throw "Ejecuta UNINSTALL.bat para elevar permisos."
}

$InstallRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$State = Join-Path $InstallRoot "install_state.json"

Write-Host ""
Write-Host "BRIO Dual Spout V17.1 - desinstalacion" -ForegroundColor Cyan

Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -like "BRIO_DUAL_SPOUT*" } |
    Stop-Process -Force -ErrorAction SilentlyContinue

if(Test-Path $State) {
    try {
        $s = Get-Content $State -Raw | ConvertFrom-Json

        $instance = $s.brio_instance_id

        if($instance) {
            $psPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\$instance\Device Parameters"

            if(Test-Path $psPath) {
                if($s.sensor_camera_mode_previous_exists -eq $true) {
                    New-ItemProperty `
                        -Path $psPath `
                        -Name SensorCameraMode `
                        -PropertyType DWord `
                        -Value ([int]$s.sensor_camera_mode_previous_value) `
                        -Force | Out-Null

                    Write-Host "SensorCameraMode restaurado a $($s.sensor_camera_mode_previous_value)." -ForegroundColor Green
                }
                else {
                    Remove-ItemProperty `
                        -Path $psPath `
                        -Name SensorCameraMode `
                        -ErrorAction SilentlyContinue

                    Write-Host "SensorCameraMode removido; originalmente no existia." -ForegroundColor Green
                }

                & pnputil.exe /restart-device "$instance" | Out-Host
            }
        }
    }
    catch {
        Write-Host "No pude restaurar automaticamente el estado de SensorCameraMode: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

$desktop = [Environment]::GetFolderPath("Desktop")
Remove-Item (Join-Path $desktop "BRIO Dual Spout V17.1.lnk") -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $desktop "BRIO Dual Spout V17.1.cmd") -Force -ErrorAction SilentlyContinue

$startFolder = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\BRIO Dual Spout"
Remove-Item $startFolder -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "No se desinstalan Visual Studio Build Tools ni VC++ Runtime porque pueden ser usados por otros programas." -ForegroundColor Yellow
Write-Host "La carpeta de la app se eliminara al cerrar este proceso." -ForegroundColor Cyan

$parent = Split-Path -Parent $InstallRoot
$cleanup = 'timeout /t 2 /nobreak >nul & rmdir /s /q "{0}"' -f $InstallRoot

Start-Process cmd.exe -ArgumentList "/d","/c",$cleanup -WindowStyle Hidden

Write-Host "Desinstalacion programada." -ForegroundColor Green
Start-Sleep -Milliseconds 500
