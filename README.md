# NinMagic Saves

**0.2.0 beta — transferencia de partidas mediante reinicio entre CFW sysMMC y emuMMC. Pendiente de validación en consola.**

[Descargar ZIP 0.2.0 beta](https://raw.githubusercontent.com/ANE-JV/ninmagic-saves/main/versions/v0.2.0/NinMagic-Saves-0.2.0.zip)

La misma aplicación se utiliza en los dos entornos. En CFW sysMMC exporta partidas originales a la microSD con aperturas de solo lectura. En emuMMC permite importar esas copias en una partida del mismo juego, eligiendo el usuario de destino y respaldando antes su progreso actual. No fusiona dos progresos.

Este recorrido requiere ejecutar Atmosphère sobre la memoria original durante la exportación. El sistema original stock no ejecuta el NRO. No se modifica el código de Atmosphère ni se instala un lector físico o módulo adicional. No se ofrece invisibilidad ante Nintendo ni garantía contra baneos.

## Instalación

Extrae NinMagic-Saves-0.2.0.zip en la raíz de la microSD, también si utilizas un descargador AIO configurado para extraer en esa raíz. El ejecutable queda en switch/NinMagicSaves/NinMagic-Saves.nro.

El paquete incluye el icono SAVES en magenta/cian neón, los recursos interiores y el canal de actualización propio. No incluye overlays, partidas, claves, imágenes NAND, módulos de Atmosphère ni cambios de arranque activos.

## Antes del primer recorrido

- En el sistema original, activa modo avión y mantén apagado el Wi-Fi. Desconecta Ethernet. Hazlo **antes** de iniciar CFW sysMMC.
- Conserva tu configuración de arranque y una copia de seguridad previa de la consola. El respaldo de partidas no sustituye un respaldo de NAND.
- Cierra los juegos. Si el destino todavía no tiene una partida, inicia ese juego una vez con el usuario elegido, guarda y ciérralo.
- Reserva espacio para las copias exportadas, el respaldo del destino y los archivos temporales.

NinMagic comprueba el entorno activo y si el Wi-Fi está deshabilitado y no existe conexión en ese momento. Eso no prueba ausencia de conexiones anteriores, no comprueba todo el estado del sistema y no evita registros locales.

## Recorrido

1. Abre NinMagic Saves en emuMMC desde Homebrew Menu.
2. Elige traer las partidas originales. Revisa el destino de arranque y confirma que dejaste el sistema original sin conexión.
3. Si falta una entrada de arranque, la aplicación puede preparar su archivo bootloader/ini/ninmagic-saves.ini, únicamente después de confirmarlo. No sobrescribe hekate_ipl.ini, no cambia el arranque automático permanente ni reemplaza Atmosphère.
4. Reinicia a CFW sysMMC. Abre nuevamente NinMagic Saves desde Homebrew Menu. **La apertura automática del NRO tras reiniciar no está implementada.**
5. Revisa que se indique sysMMC. Selecciona las partidas y expórtalas. Las distintas partidas de un mismo juego se muestran por separado.
6. Vuelve a emuMMC y abre nuevamente NinMagic Saves.
7. Elige una copia exportada y una partida de destino del mismo juego y tipo. El usuario del destino puede ser diferente al original.
8. Prepara la importación: se crea y verifica un respaldo de la partida actual antes de mostrar la confirmación.
9. Revisa juego, origen, destino y respaldo. Confirma para importar.
10. Comprueba el progreso dentro del juego. Usa la recuperación de respaldos si necesitas regresar a la partida anterior.

No apagues la consola durante una copia o importación. Si una operación queda interrumpida, conserva la carpeta de datos y revisa la recuperación antes de volver a jugar o importar.

## Modelos y configuración

La transferencia utiliza libnx y los servicios normales de Atmosphère, sin juegos o perfiles fijados. La emuMMC puede estar en partición o archivos. El modo en ejecución se consulta al sistema; no se deduce del nombre de una entrada de arranque.

El reinicio automático con destino de una sola vez depende del hardware y cargador. En Erista utiliza un payload Hekate existente, comprobado y preparado en RAM. No reemplaza atmosphere/reboot_payload.bin. En Mariko (V2, Lite, OLED) se ofrece continuar manualmente mediante el arranque de la consola; no se intenta el mecanismo exclusivo de Erista. Hekate y Atmosphère deben ser compatibles con el firmware instalado.

**No se ha probado en todos los modelos, firmwares o juegos.** Las pruebas de escritorio y la compilación no acreditan funcionamiento en una consola física. Algunos juegos incluyen identificadores de cuenta o formatos de partidas específicos de una versión. Esta aplicación no adapta automáticamente esos datos internos.

## Conservación de datos

- En sysMMC no se ofrece importar, restaurar, borrar o crear partidas; el origen se abre en modo de solo lectura.
- Solo se importa en emuMMC identificada, con el juego cerrado y un destino existente del mismo Title ID y tipo de guardado.
- Los archivos exportados se verifican con SHA-256. El respaldo del destino se verifica antes del reemplazo.
- El destino se vuelve a comprobar después de la revisión. Si cambió, hay que preparar otra importación.
- Los registros y copias permiten recuperar una importación interrumpida. No se restaura automáticamente al abrir el NRO.
- Las copias de transferencia pueden importarse en otra consola. La recuperación de un respaldo de destino queda vinculada a su entorno original para evitar restauraciones accidentales en otra emuMMC.
- Conserva config/NinMagicSaves/ mientras haya operaciones pendientes o respaldos que quieras mantener.

## Actualizaciones

Canal independiente: <https://raw.githubusercontent.com/ANE-JV/ninmagic-saves/main/update.txt>.

Desde emuMMC, abre Actualizar para consultar y descargar una versión nueva. Se comprueban identidad del NRO, versión, tamaño y SHA-256 antes de pedir confirmación. Se conserva el ejecutable anterior. Cierra y vuelve a abrir la aplicación después de instalar.

**El actualizador de red está deshabilitado en sysMMC y en entornos no identificados.** Una consulta fallida se muestra como error, no como «última versión».

## Desarrollo y verificación

Requiere devkitPro/devkitA64, libnx, libcurl, zlib y Python 3. Ejecuta tools/build.ps1 -Preview, tools/test.ps1 y python tools/package.py desde PowerShell. El ZIP y los archivos de publicación quedan en dist/publication/. Estos scripts no acceden a la consola ni publican datos.

reader-prototype/ conserva investigación anterior sobre acceso físico desde emuMMC sin reiniciar. **Ese método se abandonó para este flujo; su cliente no se compila en este NRO y no se instala.**

El código del programa se distribuye con GPL-2.0-only. Véanse LICENSE y third_party/ para los avisos de las bibliotecas, la fuente y el cliente de reinicio de referencia. Los archivos de marca conservan su titularidad separada.

Fuentes: [libnx](https://github.com/switchbrew/libnx), [Hekate](https://github.com/CTCaer/hekate), [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere). No se publican partidas, claves, identificadores privados ni archivos de inspección de una consola.
