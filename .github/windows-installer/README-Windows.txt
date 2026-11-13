MANGOHUD PARA WINDOWS — PORT EXPERIMENTAL x64
================================================

Este instalador coloca los archivos del port de MangoHud para Windows en el
equipo. Es una compilacion experimental, no una version oficial ni una version
con todas las APIs verificadas en juegos reales.

INSTALACION Y DESINSTALACION
----------------------------
Ejecuta MangoHud-Windows-Setup-x64.exe y sigue el asistente. Se instala por
defecto en la carpeta local de tu usuario:
%LOCALAPPDATA%\Programs\MangoHud
Para quitarlo, usa Configuracion de Windows > Aplicaciones > Aplicaciones
instaladas.

PANEL DE CONTROL
----------------
Abre MangoHud Control desde el menu Inicio. El panel detecta aplicaciones
abiertas y, con el seguimiento automatico activado, conecta el hook OpenGL al
proceso x64 que esta en primer plano (por ejemplo Firestorm-Releasex64.exe).
No inicia una segunda instancia de Firestorm ni usa MangoHud.exe como proceso
auxiliar: carga bin\MangoHud.dll dentro del proceso seleccionado y controla
el HUD con un evento por PID.

El seguimiento automatico es fijo: no hay lista ni selector manual de aplicaciones.
Para Direct3D 11/12 Control carga su proxy DXGI dentro del proceso activo; no copia ni reemplaza dxgi.dll en la carpeta del juego.
Control activa OpenGL directamente y carga el proxy DXGI para aplicaciones Direct3D 11/12 ya abiertas.
Pausar o salir de Control oculta el HUD; no cierra ni termina la aplicación. Haz
doble clic en el icono de MangoHud para volver a abrir el panel; con clic
derecho puedes cerrar MangoHud por completo.

Control y la aplicacion objetivo deben ejecutarse con el mismo nivel de
permisos. Si Firestorm se ejecuta como administrador, abre MangoHud Control
tambien como administrador. Si el proceso ya tiene una MangoHud.dll antigua
cargada, cierra Firestorm por completo y vuelve a abrirlo tras actualizar.

OPENGL (LANZADOR EXPERIMENTAL)
------------------------------
El lanzador opcional inicia un proceso x64 nuevo e inyecta MangoHud.dll antes
de que la aplicacion empiece a ejecutarse. Para conectar el HUD a una aplicacion
que ya esta abierta, usa MangoHud Control en lugar de este lanzador. Abre PowerShell y ejecuta:

& "$env:LOCALAPPDATA\Programs\MangoHud\bin\MangoHudLauncher.exe" "C:\Ruta\A\TuAplicacionOpenGL.exe"

El lanzador aplica automaticamente config\MangoHud.conf al proceso que inicia.
Sustituye la ruta por la del ejecutable real. No uses este lanzador con juegos
protegidos, multijugador con anti-cheat ni software que prohiba la inyeccion.

VULKAN (PRUEBA AISLADA)
-----------------------
Para probar una aplicacion Vulkan sin registrar una capa global ni modificar
el Registro de Windows, abre PowerShell y ejecuta:

& "$env:LOCALAPPDATA\Programs\MangoHud\scripts\Run-Vulkan-Test.ps1" -Executable "C:\Ruta\A\TuAplicacionVulkan.exe"

La prueba configura las variables de entorno solo para ese proceso y sus
procesos secundarios.

DIRECTX 11 Y DIRECTX 12
-----------------------
Los componentes experimentales estan incluidos, pero la integracion DirectX
todavia necesita validacion en aplicaciones reales. El archivo
DirectX\dxgi.dll se instala dentro de la carpeta de MangoHud; NO se copia
automaticamente junto a juegos y NO debe copiarse a System32 ni a carpetas de
juegos protegidos o con anti-cheat.

SKINS DEL HUD
--------------
MangoHud Control incluye ocho temas seleccionables:

- Dark: tema oscuro original del port.
- Mango: colores naranja y verde de la identidad MangoHud.
- Minimal: diseño compacto original del port.
- Glass: panel translucido original del port.
- Catppuccin Latte, Frappe, Macchiato y Mocha: paletas comunitarias adaptadas
  desde Catppuccin para MangoHud.

Dark sigue siendo la apariencia predeterminada. Dark y Minimal se mantienen
sin cambios. Los temas Catppuccin conservan su atribucion y licencia MIT en
skins\Catppuccin-LICENSE.txt; el proyecto original esta en:
https://github.com/catppuccin/mangohud

El tema elegido se guarda y se aplica al HUD conectado al recibir el siguiente
frame; tambien queda seleccionado para los siguientes juegos. Los archivos
de cada skin estan en la carpeta skins.

CONFIGURACION
-------------
La plantilla de configuracion esta en config\MangoHud.conf dentro de la
carpeta de instalacion. El instalador conserva ese archivo cuando se actualiza
una instalacion existente para no sobrescribir los cambios del usuario.

ESTADO DE VALIDACION
--------------------
La compilacion MSVC x64, el panel de control y la generacion del instalador se comprueban en CI. Una
compilacion correcta no demuestra que todas las APIs funcionen en todos los
juegos. OpenGL, Vulkan y DirectX deben seguir probandose en aplicaciones x64
compatibles. Algunas funciones y metricas pueden estar incompletas. La captura ETW avanzada de FPS puede requerir permisos elevados; si no los tiene, el resto del overlay sigue funcionando.

Proyecto del port Windows:
https://github.com/artemissoulz-commits/mangohudwin
