# RiMU2000 para Android — 0.1.1 alpha

Primera aplicación independiente para Android del motor de [S-MU2000](https://github.com/tarboh/S-MU2000). Ejecuta el firmware del MU2000 y conserva la emulación del SH7043 y de los dos SWP30, incluidos los compiladores JIT ARM64 de upstream. La interfaz Android conecta ese motor con MIDI y audio del dispositivo.

La alpha está orientada a la Samsung Galaxy Tab A9 y requiere Android 8.0 o superior con CPU ARM64. El rendimiento y la latencia deben medirse en la tablet: que el código compile no garantiza ausencia de cortes con todas las canciones o sonidos.

## Cambios de 0.1.1

Esta versión reduce trabajo repetido del motor y permite distinguir la ejecución real de los JIT de su mera presencia en el APK. Conserva el firmware, la emulación de ambos SWP30, la frecuencia interna de 44.100 Hz y el conversor sinc.

- El salto del generador aleatorio usa transformaciones precalculadas e inmutables para conservar exactamente la misma secuencia. El MEG inactivo puede avanzar su estado sin recalcular los coeficientes de ese salto por cada muestra.
- El JIT MEG ARM64 especializa coeficientes estables y calcula una sola vez por muestra los LFO utilizados repetidamente, como ya hacía el backend x86-64. Una escritura del firmware invalida inmediatamente la especialización correspondiente.
- El emisor ARM64 elige MOVZ o MOVN para construir constantes de 64 bits con menos instrucciones, incluidos coeficientes negativos.
- En Android 13 o posterior, ADPF recibe el plazo y el tiempo real de cada bloque cuando el dispositivo ofrece sesiones de rendimiento. Todas las llamadas de esa API se realizan fuera del callback de audio. El diagnóstico indica si la sesión está activa; el efecto sobre frecuencia y planificación depende del dispositivo. Referencias: [guía de audio de Oboe](https://github.com/google/oboe/wiki/Using-ADPF-for-High%E2%80%90Quality-Audio-Performance) y [API nativa](https://developer.android.com/ndk/reference/group/a-performance-hint).
- El panel muestra una ventana reciente de aproximadamente un segundo, el tiempo de CPU del hilo, tiempo del motor y de conversión, núcleos observados, modo AAudio negociado y actividad/fallos reales de los JIT SH2 y MEG. El reposo del MEG esclavo se informa por separado: es una optimización válida.

**CPU app 100 % equivale al tiempo de un núcleo**, sumando los hilos de la aplicación. No representa el 100 % de toda la tablet. El porcentaje de render compara el tiempo de trabajo con la duración del bloque de audio; valores sostenidos mayores al 100 % indican que el motor no alcanza tiempo real. Aumentar el buffer puede absorber picos, pero no elimina un déficit sostenido.

Para comparar esta actualización, repetir primero la prueba con 2048 frames, la misma salida y el mismo sonido. Observar unos segundos en reposo y luego con notas sostenidas; comparar el render reciente y el crecimiento de xruns.

### Validación de esta actualización

Se ejecutaron el backend ARM64 y Bionic con QEMU, incluidos los selftests del emisor y los auxiliares MEG. Siete casos MIDI (`piano`, `effects`, `dense`, `ports`, `lofi`, `fxchange`, `ccramp`) produjeron PCM idéntico byte por byte al comparar las variantes general, especializada y especializada con reutilización de LFO. El test del generador compara 6.176 combinaciones con extracciones secuenciales, incluidos límites y la ruta para saltos mayores. Los cuatro casos básicos también conservan los WAV completos frente a la referencia x86-64 anterior.

Estas pruebas usan ROMs externas al repositorio. Verifican exactitud y ejecución de código ARM64; QEMU no permite predecir la velocidad de la A9 ni verifica su AAudio, controlador MIDI físico o respuesta a ADPF. Las cifras reales deben obtenerse en la tablet.

## ROMs

El APK y este repositorio no incluyen firmware ni ROMs de Yamaha. Importa tus archivos desde el selector de documentos de la aplicación:

| Archivo | Tamaño exacto |
| --- | ---: |
| `mu2000_flash.bin` | 4 194 304 bytes |
| `xv364a0.ic49` | 8 388 608 bytes |
| `xv365a0.ic50` | 8 388 608 bytes |
| `xw848a0.ic53` | 8 388 608 bytes |
| `xw849a0.ic54` | 8 388 608 bytes |
| `sin-table.bin` (si se dispone de ella) | 65 536 bytes |

Los archivos se copian al almacenamiento privado de la aplicación. Si no se importa la tabla de senos, se utiliza el sustituto matemático del motor. No añadas las ROMs al repositorio, a los artefactos de Actions ni a los informes de errores. Las instrucciones de extracción están en [la documentación de upstream](../README.md#roms).

## Primer ensayo en la tablet

1. Instala el APK ARM64 e importa las ROMs.
2. Conecta el controlador USB-MIDI, selecciónalo en la aplicación y elige la salida de auriculares o altavoz de la tablet.
3. Empieza con un buffer de **1024 frames**, inicia el audio y prueba unas notas. Si hay cortes, prueba 2048; si funciona estable, prueba 512.
4. Anota el render, los callbacks fuera de presupuesto y los xruns. Son medidas distintas; el porcentaje de render no equivale a la latencia entre tecla y sonido.

El tamaño elegido es una solicitud a Android. El dispositivo puede negociar otro tamaño de buffer o de callback; el diagnóstico muestra los valores efectivos. Si rechaza el callback solicitado, la aplicación permite que Android elija su tamaño y lo informa allí.

La alpha detiene el audio al dejar la aplicación. El botón de pánico permite cortar las notas y liberar el pedal. Los mensajes del controlador conservan su canal MIDI. El parser reconstruye los mensajes fragmentados, incluido running status, antes de mezclarlos con los controles de pantalla. En esta versión admite mensajes SysEx de hasta **65 536 bytes**; rechaza completos los que superan ese tamaño y muestra el contador en el diagnóstico MIDI.

## Compilar

Herramientas fijadas para reproducir la compilación:

| Herramienta | Versión |
| --- | --- |
| JDK | 17 |
| Gradle | 8.11.1 (wrapper incluido) |
| Android Gradle Plugin | 8.9.2 |
| Android SDK / Build Tools | 35 / 35.0.0 |
| Android NDK | 28.2.13676358 |
| CMake | 3.22.1 |

Con Android Studio, abre esta carpeta `android/`. Desde una terminal con `JAVA_HOME` y `ANDROID_HOME` configurados:

```sh
sdkmanager 'platforms;android-35' 'build-tools;35.0.0' 'ndk;28.2.13676358' 'cmake;3.22.1'
cd android
./gradlew --no-daemon :app:assembleDebug
```

El resultado queda en `app/build/outputs/apk/debug/app-debug.apk`. Es una alpha firmada con la clave debug local y contiene código nativo optimizado. El NDK r28 y el empaquetado actual generan bibliotecas compatibles con páginas de 16 KiB.

No se incluye una clave privada en el repositorio. Una compilación realizada en otra máquina o en un runner nuevo puede usar otra clave debug: Android podría exigir desinstalar la alpha anterior, perdiendo sus datos internos, antes de instalarla. Para distribuir actualizaciones conservando los datos, el propietario deberá configurar una clave estable de firma en su entorno o en secretos de CI.

## GitHub Actions

El workflow **Android alpha** compila cuando se actualiza la rama `android` y permite una ejecución manual. Antes del APK ejecuta la verificación del motor de upstream sin ROMs, las pruebas del parser MIDI y de la cola concurrente; también ejecuta Android Lint. En **Actions → Android alpha → ejecución terminada → Artifacts**, descarga `RiMU2000-0.1.1-arm64`, que contiene el APK y el código fuente de ese commit.

La compilación de CI no necesita ROMs y no verifica sonido. Las pruebas con firmware y mediciones de rendimiento se realizan por separado en un dispositivo.

## Licencias

Se conservan las licencias y avisos del motor original en [LICENSE](../LICENSE) y [NOTICE.txt](../NOTICE.txt). Gradle copia ambos archivos al APK bajo `assets/licenses/`; se pueden consultar desde la aplicación. Los archivos de la aplicación Android son una adaptación para este port. Los datos extraídos del hardware no forman parte de este código fuente.
