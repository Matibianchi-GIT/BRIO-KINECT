BRIO DUAL SPOUT V17.1 - AUTOINSTALLER 17.1.2
=====================================

QUE HACE
--------
Ejecuta SETUP_AND_RUN.bat.

El instalador solicita elevacion de administrador y luego comprueba o instala
automaticamente las dependencias antes de iniciar la camara:

1. Comprueba la Logitech BRIO fisica VID_046D PID_085E / MI_00.
2. Comprueba SensorCameraMode.
   - Si no es 2, hace backup del registro, fija SensorCameraMode=2 y reinicia
     solamente la interfaz de camara.
3. Comprueba Visual Studio 2022 Build Tools C++ y Windows SDK.
   - Si faltan, descarga el bootstrapper oficial de Microsoft e instala la
     workload Microsoft.VisualStudio.Workload.VCTools con componentes recomendados.
4. Comprueba Microsoft Visual C++ Redistributable x64.
   - Si falta, descarga el instalador oficial de Microsoft.
5. Descarga la release oficial mas reciente de Spout2 desde leadedge/Spout2.
6. Compila BRIO Dual Spout V17.1.
7. Crea accesos directos en Escritorio y Menu Inicio.
8. Arranca la app.

PERFIL DE CAMARA ACTUAL
-----------------------
FaceAuth : 0x2 ALT_FRAME_ILLUMINATION
IR       : 340x340 L8 @30
RGB      : 800x600 YUY2 @30
Orden    : IR primero -> 100 ms -> RGB

SPOUT
-----
Senders:
  BRIO_RGB
  BRIO_IR

TouchDesigner:
  Syphon Spout In TOP -> BRIO_RGB
  Syphon Spout In TOP -> BRIO_IR

INSTALACION
-----------
1. Descomprime TODO el ZIP en una carpeta.
2. Doble click en SETUP_AND_RUN.bat.
3. Acepta el UAC.
4. Si Camera, OBS, Logi Tune o TouchDesigner estan usando la BRIO, el instalador
   te pedira que los cierres.
5. En la primera instalacion puede tardar bastante si tiene que descargar
   Visual Studio Build Tools.

DESTINO
-------
%LOCALAPPDATA%\BRIO_DUAL_SPOUT\V17_1

DESINSTALAR
-----------
En la carpeta instalada ejecuta UNINSTALL.bat.

El desinstalador:
- detiene BRIO Dual Spout;
- restaura SensorCameraMode al valor que existia antes de la primera instalacion;
- reinicia la interfaz de la BRIO;
- elimina accesos directos y archivos de la app.

No elimina Visual Studio Build Tools ni Visual C++ Runtime, porque son
dependencias compartidas que otros programas pueden utilizar.

IMPORTANTE
----------
- No flashea firmware.
- No instala drivers Logitech no oficiales.
- El instalador necesita Internet si falta alguna dependencia.
- Los binarios generados localmente no estan firmados digitalmente; Windows
  puede mostrar una advertencia de SmartScreen.

CAMBIO 17.1.1
-------------
Se elimino la dependencia de WScript.Shell para crear accesos directos.
En algunos Windows 11 esa llamada COM devuelve E_INVALIDARG:
"El valor no esta dentro del intervalo esperado" aunque la aplicacion ya haya
compilado correctamente.

Ahora el instalador crea lanzadores .cmd en Escritorio y Menu Inicio, evitando
ese fallo. Si ya corriste la version anterior, vuelve a ejecutar
SETUP_AND_RUN.bat: las dependencias ya instaladas se detectan y se reutilizan.

CAMBIO 17.1.2
-------------
Corregido el fallo:
  No se puede convertir 'System.Object[]' al tipo 'System.String'
  requerido por el parametro 'FilePath'.

Causa:
La salida de cmd.exe/cl.exe (incluyendo el banner del Developer Command Prompt)
estaba siendo devuelta por la funcion Build-App junto con la ruta del EXE.
Por eso $exe se convertia en un array.

Correccion:
- la salida del compilador ahora va a pantalla con Out-Host y no forma parte
  del valor de retorno;
- Build-App devuelve explicitamente un unico string;
- se agrega una validacion defensiva antes de Start-Process.

Si ya ejecutaste versiones anteriores, vuelve a correr SETUP_AND_RUN.bat:
las dependencias instaladas se reutilizan.
