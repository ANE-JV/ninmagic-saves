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

La transferencia prueba origen de solo lectura, coincidencia de juego/tipo, cuentas distintas, capacidad de destino, hashes, rutas, enlaces, copias dañadas, cambios posteriores a la revisión, fallos de escritura/commit y recuperación tras reiniciar el proceso de prueba. El reinicio prueba configuración RAW/archivo, selección CFW, rechazo de stock y destinos ambiguos, confirmaciones y payload Hekate en memoria. Ninguna prueba solicita un reinicio real.

La 0.2.0 implementa exportación/importación y el recorrido de reinicio. Estos resultados sintéticos no demuestran que esas operaciones funcionen en una consola física. El primer ensayo real permanece pendiente. Véase docs/validation-0.2.0.json.
