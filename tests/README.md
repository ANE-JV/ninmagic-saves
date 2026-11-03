# Verificación local

Las pruebas usan archivos sintéticos y carpetas temporales; no necesitan una consola.

Ejecución completa y reproducible desde la carpeta del proyecto:

```powershell
./tools/build.ps1 -Preview
./tools/test.ps1
```

El segundo comando compila y ejecuta las suites de actualizador, HTTPS, interfaz, transferencia y reinicio, incluida la inspección del NRO compilado. También conserva las pruebas del antiguo cliente/prototipo del lector, que no se compilan dentro de esta aplicación. Aborta ante cualquier fallo y no omite casos. Configura los enlaces de MSYS solo durante la ejecución y restaura el entorno anterior al terminar.

El script normaliza las rutas a barras `/` para GCC de MSYS/Cygwin: una ruta de entrada con barras Windows puede hacer que un include relativo se resuelva desde otra carpeta y pruebe un proyecto distinto.

Desde PowerShell, con devkitPro en `C:\devkitPro`:

```powershell
$cxx = 'C:/devkitPro/msys2/usr/bin/g++.exe'
$env:MSYS = 'winsymlinks:lnk'
& $cxx -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude tests/app_update_test.cpp -o "$env:TEMP/ninmagic-saves-update-test.exe"
& "$env:TEMP/ninmagic-saves-update-test.exe"
& $cxx -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude tests/update_network_test.cpp -o "$env:TEMP/ninmagic-saves-network-test.exe"
& "$env:TEMP/ninmagic-saves-network-test.exe"
```

`winsymlinks:lnk` limita el ajuste al entorno del proceso. Hace que MSYS represente enlaces como enlaces en las pruebas, sin necesitar privilegios de administrador; su modo predeterminado puede copiar el archivo al crear un enlace.

La suite del actualizador comprueba manifiestos, identidad interna del NRO, SHA-256, versiones anteriores, rutas exclusivas, descargas fallidas, interrupciones, devolución del NRO anterior y conservación de copias. La suite HTTPS comprueba URL, aislamiento de destinos y límites de recepción. No prueba una conexión real desde Nintendo Switch.

El test del actualizador también acepta un NRO y su SHA-256 como dos argumentos para inspeccionar el ejecutable compilado. `tools/build.ps1 -Preview` compila y verifica estructuralmente el NRO y genera imágenes de interfaz con datos explícitos de ejemplo.

La transferencia prueba origen de solo lectura, coincidencia de Title ID/tipo/índice, cuentas distintas, capacidad de destino, hashes, rutas, enlaces, copias dañadas, cambios posteriores a la revisión, fallos de escritura/commit y recuperación tras reiniciar el proceso de prueba. El reinicio prueba configuración RAW/archivo, selección CFW, rechazo de stock y destinos ambiguos, confirmaciones y payload Hekate en memoria. Ninguna prueba solicita un reinicio real.

En la 0.2.1, la detección de red es informativa: una conexión detectada o un estado desconocido no deben bloquear exportación, preparación o reinicio. Las pruebas de política distinguen estos casos de un entorno NAND desconocido o un juego abierto, que siguen bloqueados. La interfaz presenta el aviso antes de que A confirme crear el acceso o reiniciar hacia sysMMC CFW, y mantiene avisos al acceder al original. No usa una casilla Y de modo avión activo; confirmar la acción no equivale a verificar ese ajuste. Activarlo después de arrancar no elimina una posible conexión inicial; ninguna prueba acredita invisibilidad ni ausencia de riesgo de baneo.

La 0.2.4 conserva exportación/importación, backup verificado, recuperación y el recorrido de reinicio. La red sigue siendo informativa desde la 0.2.1. Estos resultados sintéticos no demuestran que el recorrido completo funcione en una consola física. El informe generado para esta versión debe consultarse en docs/validation-0.2.4.json; no sustituye una prueba real de exportación, retorno e importación.

La 0.2.2 añade regresiones para títulos decorativos de Hekate con sufijos, espacios, CRLF y sin llave final, conservando la separación de claves, los índices y la detección de IDs duplicados. Los errores reales incluyen archivo y línea.

La 0.2.3 corrige la consulta de capacidad de partidas abiertas en solo lectura. La selección del destino no compara versiones del juego ni exige el mismo usuario: conserva la coincidencia exacta de Title ID, tipo e índice. Las pruebas sintéticas no pueden demostrar que un juego antiguo reconozca un guardado creado con una versión nueva; NinMagic copia los datos sin convertir su formato.

La 0.2.4 prueba escrituras por tramos con un modelo transaccional de journal limitado: archivo mayor que el journal, confirmaciones parciales, fallo que exige desmontar, fallo durante la recuperación, reinicio con copia parcial persistente y respaldo vacío que ya coincide con el destino. Comprueba además que un journal desconocido o demasiado pequeño impide modificar el guardado. El modelo reproduce estas condiciones de prueba; no mide el espacio temporal real ni acredita el resultado en todos los sistemas de archivos de Switch.
