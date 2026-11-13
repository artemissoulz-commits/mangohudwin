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

OPENGL (LANZADOR EXPERIMENTAL)
------------------------------
El lanzador inyecta MangoHud.dll en un proceso x64 antes de que la aplicacion
empiece a ejecutarse. Abre PowerShell y ejecuta:

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

CONFIGURACION
-------------
La plantilla de configuracion esta en config\MangoHud.conf dentro de la
carpeta de instalacion. El instalador conserva ese archivo cuando se actualiza
una instalacion existente para no sobrescribir los cambios del usuario.

ESTADO DE VALIDACION
--------------------
La compilacion MSVC x64 y la generacion del paquete se comprueban en CI. Una
compilacion correcta no demuestra que todas las APIs funcionen en todos los
juegos. OpenGL, Vulkan y DirectX deben seguir probandose en aplicaciones x64
compatibles. Algunas funciones y metricas pueden estar incompletas.

Proyecto del port Windows:
https://github.com/artemissoulz-commits/mangohudwin
