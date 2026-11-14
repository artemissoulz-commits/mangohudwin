YORHUD — WINDOWS PERFORMANCE OVERLAY
=====================================

YORHUD es un panel de rendimiento para Windows basado en el motor MangoHud y adaptado al port de este repositorio. El producto incluye una interfaz propia, temas compatibles .conf, perfiles nativos y una identidad visual propia.

INSTALACIÓN Y DESINSTALACIÓN
----------------------------
Ejecuta YorHud-Setup-x64.exe. Se instala por defecto en:
%LOCALAPPDATA%\Programs\YorHud

Para desinstalarlo, usa Configuración de Windows > Aplicaciones > Aplicaciones instaladas. El desinstalador cierra YORHUD y desactiva sus señales antes de borrar los archivos. No fuerza el cierre de los juegos. Si un juego mantiene una DLL cargada, Windows podrá eliminarla al cerrar el juego o, si sigue bloqueada, al reiniciar.

PANEL Y SEGUIMIENTO
-------------------
Abre YORHUD desde el menú Inicio. El panel muestra la mascota oficial y sigue automáticamente la aplicación x64 compatible que está en primer plano. Puedes minimizarlo a la bandeja; no inicia una segunda instancia del juego ni termina procesos de usuario.

Para Direct3D 11/12 se carga el proxy DXGI dentro del proceso. Para OpenGL se carga el hook WGL. El port y las APIs siguen siendo experimentales y necesitan pruebas en aplicaciones reales.

TEMAS Y SKINS DE LA COMUNIDAD
-----------------------------
YORHUD incluye estos temas .conf:
- YORHUD: paleta plata, grafito y rosa.
- Dark: tema oscuro.
- Mango: naranja y verde.
- Minimal: lectura compacta.
- Glass: panel translúcido.

Puedes añadir más skins compatibles con MangoHud copiando sus archivos .conf a la carpeta:
%LOCALAPPDATA%\Programs\YorHud\skins

Después pulsa ACTUALIZAR en el selector de apariencia. YORHUD descubre los archivos .conf automáticamente. Los archivos deben usar opciones que admita la versión de MangoHud incluida. Un tema externo que necesita un layout o un renderizador propio no se convierte en compatible por copiar solamente su .conf; habría que adaptar ese código al port.

PERFILES NATIVOS — F10
----------------------
El HUD conserva el sistema nativo de presets de MangoHud. Pulsa F10 dentro de la aplicación para alternar los perfiles configurados en config\presets.conf:
- Stream: lectura horizontal compacta.
- Pro: estadísticas ampliadas.
- Hardware: temperaturas, frecuencias y consumo.
- Benchmark: métricas de frametime y estadísticas detalladas.

El archivo presets.conf se incluye con YORHUD y se selecciona dentro del proceso del juego mediante MANGOHUD_PRESETSFILE. Los perfiles no son skins de dibujo: cambian qué métricas y disposición muestra el HUD con las opciones nativas disponibles.

PRUEBAS Y LIMITACIONES
---------------------
La compilación MSVC x64 y la generación del instalador se comprueban mediante CI. Una compilación correcta no demuestra que todas las APIs o métricas funcionen en todos los juegos. OpenGL, Vulkan y DirectX necesitan validación de ejecución en aplicaciones x64 compatibles. Algunas funciones pueden estar incompletas y la captura ETW avanzada de FPS puede requerir permisos elevados.

PROYECTO
--------
https://github.com/artemissoulz-commits/mangohudwin
