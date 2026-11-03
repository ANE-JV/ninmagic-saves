# Reinicio entre emuMMC y CFW sysMMC — 0.2.4 beta

Implementación de NinMagic Saves 0.2.4 beta. Las pruebas con archivos sintéticos y la compilación no sustituyen la validación del recorrido completo en una consola real. Las pruebas de escritorio descritas aquí no solicitan reinicios físicos.

## Qué hace

La NRO consulta a Atmosphere mediante SPL 65007 para distinguir la NAND activa. No deduce el entorno por nombres del menú, por la configuración de la SD, ni por el contenido de una GPT clonada. SPL devuelve 0 para sysMMC y 1 para emuMMC; otros valores o errores quedan como desconocidos.

Al extraer partidas, el destino del reinicio es **CFW sobre sysMMC**. Stock/OFW no ejecuta esta NRO. Después de cada arranque hay que volver a abrir NinMagic Saves desde hbmenu. No hay reapertura automática ni acceso simultáneo a las dos NAND.

El código revisa las entradas existentes de Hekate por su contenido. Excluye stock, payloads, Linux, destinos emuMMC sobreescritos, instrucciones duplicadas o ambiguas y componentes personalizados no revisados. Acepta `pkg3=atmosphere/package3` o el alias `fss0` y una selección inequívoca mediante `emummcforce=1` o `emummc_force_disable=1`. Las etiquetas de las entradas no conceden compatibilidad.

Desde 0.2.2, los títulos decorativos `{...}` se interpretan como separadores y no como opciones de arranque, incluso si carecen de llave final o llevan texto después de ella, como admite el lector oficial de Hekate. No aportan claves ni IDs a una entrada y no aumentan su índice. Las secciones `[config]` de INI externos se omiten antes de contar opciones o buscar IDs, como hace Hekate. Los errores de sintaxis restantes identifican el archivo y la línea; esta lectura no modifica el INI del usuario.

## Preparar acceso, con confirmación en pantalla

Cuando falta una entrada, la aplicación puede crear únicamente:

`sdmc:/bootloader/ini/ninmagic-saves.ini`

```ini
[NinMagic Saves - sysMMC CFW]
id=nmsvSys
pkg3=atmosphere/package3
emummc_force_disable=1

[NinMagic Saves - volver a emuMMC]
id=nmsvEmu
pkg3=atmosphere/package3
emummcforce=1
```

**La entrada no desactiva Internet**. Antes del arranque CFW sysMMC, activa el modo avión en el sistema original y desconecta cualquier red por cable. La aplicación no puede comprobar esa configuración desde emuMMC. Activar modo avión después del arranque no elimina una posible conexión inicial.

La pantalla de preparación o revisión presenta este aviso antes de continuar hacia sysMMC CFW. A confirma crear el acceso o reiniciar después de leerlo; no hay una casilla Y ni una declaración de modo avión activo. Esa confirmación no acredita que el original esté sin conexión. Al acceder a sysMMC, la aplicación mantiene el aviso; no bloquea la exportación por el estado de red.

No se modifica `hekate_ipl.ini`, el autoboot persistente, `emummc.ini`, `exosphere.ini`, `package3` ni `atmosphere/reboot_payload.bin`. No se añaden sigpatches, sysmodules ni drivers. No se copian opciones `cal0blank` de otro perfil. Las opciones globales que ya existan en Atmosphere siguen aplicándose; no se fuerza su activación ni su desactivación y no se modifica PRODINFO.

Un archivo propio idéntico se conserva. Un archivo existente con otro contenido se rechaza y se conserva sin sobrescribir: no necesita una copia de seguridad porque no se altera. Una escritura interrumpida deja `.pending` para revisión manual y bloquea una nueva preparación. Para retirar las dos entradas, se puede quitar este único INI propio desde un gestor de archivos, con la consola fuera de una transición; no hace falta editar las entradas anteriores.

La validación de emuMMC admite rutas relativas personalizadas y configuraciones RAW o por archivos. Lee únicamente su INI y, para RAW, el marcador SD `raw_based`; para archivos comprueba existencia/tamaño de los archivos esperados. No lee sectores eMMC ni el contenido de BOOT0, BOOT1 o del archivo `00`. Estas comprobaciones no sustituyen la identificación real del entorno tras arrancar.

## Comprobación de red y juegos abiertos

No existe una consulta de modo avión usada por esta implementación. Se consulta si NIFM indica Wi-Fi desactivada y devuelve exactamente `0xD46ED`, documentado por libnx como «ni conectando ni conectado». Otros errores se muestran como desconocidos, nunca como «sin conexión». Desde la 0.2.1, estos resultados son informativos: una red conectada, una conexión en curso o un estado desconocido **no bloquean la preparación, el reinicio ni la exportación**. La interfaz Ethernet habilitada, por sí sola, tampoco prueba que exista cable o conexión.

Esta consulta describe el momento actual. No asegura que Internet no haya estado activo antes o que no pueda activarse después. Para entrar en sysMMC CFW se presenta el aviso sobre el modo avión y la posible conexión inicial antes de confirmar la acción con A; no se exige una detección de red favorable. No se promete invisibilidad ni protección garantizada contra baneos.

Se exige que no haya un juego abierto. PM debe devolver `ProcessNotFound`, o identificar inequívocamente el proceso de la propia NRO en modo aplicación. Se vuelve a consultar justo antes de solicitar el reinicio.

## Modelos de consola y reinicio

| Familia comprobada | Comportamiento |
| --- | --- |
| Erista (Nx/Copper) | Ruta de un solo uso mediante payload Hekate instalado, si pasa las comprobaciones. |
| Mariko (Iowa/Hoag/Calcio/Aula: V2, Lite, OLED) | Instrucciones para entrar manualmente en Hekate y elegir la entrada. No se ejecuta el mecanismo Erista. |
| Modelo desconocido | Selección manual; no se promete un método de reinicio automático. |

Se prefiere `bootloader/update.bin`; se admite `atmosphere/reboot_payload.bin` solamente si su contenido se reconoce como Hekate. Se comprueba tamaño máximo de IRAM (0x24000), metadatos ICTC en 0x118 y versión 6.x, compatible con la ABI usada. Es una comprobación estructural, **no una firma criptográfica ni una prueba de que el arranque vaya a funcionar**. Versiones o formatos desconocidos pasan a la ruta manual.

En Erista se modifica sólo una copia en RAM del bloque `boot_cfg` en 0x94. Se borra el bloque de 0x84 bytes para impedir que queden opciones anteriores, se utiliza `FORCE_AUTOBOOT | FROM_ID` y el ID único seleccionado. Para una entrada existente sin ID puede usarse su índice y lista, tras revisar la configuración completa. Un ID inválido o duplicado deja disponible la selección manual y nunca se envía como selección automática.

La confirmación final vuelve a comprobar el entorno, que no haya un juego abierto, los contenidos INI y el payload. Un cambio en el destino o en la configuración invalida la revisión. La red se consulta para informar; un estado conectado o desconocido no invalida el plan ni impide continuar tras leer el aviso. El cliente usa el puerto oficial `bpc:ams`, comando 65001, y `spsmShutdown(true)`. Si el reinicio comunica un error después de preparar el payload, intenta retirar la selección temporal restaurando en RAM el payload revisado sin parchear. Nunca escribe el payload de la SD.

No hay un driver para leer NAND desde emuMMC. El código de reinicio no enumera ni importa partidas. La exportación/importación pertenece al módulo de transferencia, y sólo comienza después de comprobar la NAND en la nueva ejecución. La importación exige el mismo Title ID, tipo e índice de guardado, con destino existente y respaldo verificado; admite usuarios y versiones del juego distintos. Las partidas de usuario y los datos compartidos de la consola se muestran y transfieren por separado; no se mezclan ni convierten entre tipos. No convierte formatos: una versión antigua del juego puede no reconocer un guardado más nuevo.

## Límites de seguridad y compatibilidad

Arrancar CFW en sysMMC no es lo mismo que iniciar el modo original stock. Ningún componente garantiza invisibilidad, ausencia de registros, ausencia de baneo ni compatibilidad universal de firmware/bootloader. Tampoco impide que los componentes que el usuario ya tenga instalados se ejecuten durante CFW sysMMC. Una consola sin Hekate adecuado necesita configurar su ruta de arranque antes de usar este flujo.

Las entradas generadas no incluyen configuraciones privadas ni identificadores de consola. Los tokens de revisión permanecen sólo en memoria y no se publican. La aplicación no sube archivos, claves ni partidas como parte de este flujo.

## Pruebas realizadas

`tests/reboot_workflow_test.cpp` compila de forma independiente con MSYS g++ y `-std=c++17 -Wall -Wextra -Werror`. Usa un directorio temporal creado por la prueba. Cubre selección por semántica, rechazo de stock/overrides, IDs duplicados incluso en entradas excluidas, sintaxis específica de Hekate, payload incorrecto, límites de versión/tamaño, modificación exclusiva del bloque RAM, confirmaciones, archivo existente diferente, preparación idempotente, cambios después de la revisión, estados de red informativos sin bloqueo, rechazo de juego abierto, Mariko y configuración emuMMC RAW/archivo. No ejecuta reinicios reales.

`source/reboot_workflow.cpp` también compila para AArch64 con el SDK libnx local (`-Wall -Werror`, sin excepciones ni RTTI). Falta probar el arranque real y el retorno con consentimiento del usuario y una configuración comprobada.

## Fuentes primarias y licencia

La implementación es GPL-2.0-only. Se han seguido protocolos y constantes documentados por los proyectos siguientes; no se distribuyen sus binarios modificados:

- [Hekate: opciones INI y boot storage](https://github.com/CTCaer/hekate/blob/master/README.md).
- [Hekate: boot_cfg_t y metadatos](https://github.com/CTCaer/hekate/blob/master/bdk/utils/types.h).
- [Hekate: parser INI real](https://github.com/CTCaer/hekate/blob/master/bdk/utils/ini.c).
- [Hekate: búsqueda por ID e inicio](https://github.com/CTCaer/hekate/blob/master/bootloader/main.c).
- [Hekate: configuración emuMMC RAW/archivo](https://github.com/CTCaer/hekate/blob/master/bootloader/storage/emummc.c).
- [Hekate: configuración existente de exosphere](https://github.com/CTCaer/hekate/blob/master/bootloader/hos/secmon_exo.c).
- [Hekate Toolbox: mecanismo Erista y modo restringido Mariko](https://github.com/WerWolv/Hekate-Toolbox/blob/master/source/gui/gui_hekate.cpp).
- [Atmosphere: cliente oficial de reboot-to-payload](https://github.com/Atmosphere-NX/Atmosphere/blob/master/troposphere/reboot_to_payload/source/main.c) y [puerto/comando](https://github.com/Atmosphere-NX/Atmosphere/blob/master/troposphere/reboot_to_payload/source/ams_bpc.c).
- [Atmosphere: resultados de PM](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/results/pm_results.hpp).
- [libnx: NIFM y resultado sin conexión](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/nifm.h).

