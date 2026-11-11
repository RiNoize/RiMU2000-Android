# RiMU2000 para Android — 0.1.2 alpha

Primera aplicación independiente para Android del motor de [S-MU2000](https://github.com/tarboh/S-MU2000). Ejecuta el firmware del MU2000 y conserva la emulación del SH7043 y de los dos SWP30, incluidos los compiladores JIT ARM64 de upstream. La interfaz Android conecta ese motor con MIDI y audio del dispositivo.

La alpha está orientada a la Samsung Galaxy Tab A9 y requiere Android 8.0 o superior con CPU ARM64. El rendimiento y la latencia deben medirse en la tablet: que el código compile no garantiza ausencia de cortes con todas las canciones o sonidos.

## Cambios de 0.1.2

Esta actualización responde a una medición en la A9 con JIT activo, pero 54,52 ms de trabajo por cada bloque de 42,67 ms. El motor consumía el 117,5 % del presupuesto y la conversión otro 10,3 %. El modo de CPU y el conversor se pueden evaluar por separado; todavía es necesario comprobar el rendimiento en la tablet.

- **CPU del motor:** permite elegir **Automático (Android)**, el valor inicial, o **Núcleos de rendimiento**. El segundo modo consulta la capacidad que publica el dispositivo y solicita el grupo de mayor capacidad dentro de la máscara permitida al hilo de audio. Si no hay datos completos de capacidad, puede usar la frecuencia máxima como aproximación explícita. Si no hay información suficiente o no se distinguen grupos, conserva la selección de Android y muestra el motivo. No contiene números de CPU fijados para un modelo concreto.
- La detección de CPU se hace antes de iniciar el stream. Las llamadas para aplicar y comprobar afinidad se hacen sobre el propio hilo, una sola vez al comenzar cada hilo de callback. No cambia su política ni prioridad, y no accede a sysfs en el procesamiento habitual. El panel muestra el criterio y las máscaras permitida, solicitada y efectiva, junto con cualquier error. ADPF sigue activo cuando está disponible.
- **Conversión sinc de 64 coeficientes precalculados por fase.** Para 44.100 → 48.000 Hz hay 160 fases. Se conserva la tabla del filtro, el corte, la acumulación en doble precisión y la normalización especial del arranque. El avance racional evita la deriva acumulada de la posición. Esta ruta sólo se activa explícitamente en Android; las otras plataformas conservan su implementación. Las frecuencias con ciclos mayores a 2.048 fases vuelven al conversor genérico.
- El diagnóstico reconoce `SCHED_RESET_ON_FORK` al mostrar la política del hilo; así una política FIFO no aparece como `?`. Es una corrección de la etiqueta, sin cambios de prioridad.
- El contenido desplazado queda recortado al espacio útil, evitando que el título invada la barra de estado.

La afinidad es una prueba reversible, no una promesa de mejora en todos los dispositivos. Reduce la libertad de Android para repartir ese hilo y puede cambiar consumo o temperatura. Volver a **Automático (Android)** y reiniciar el audio restaura el comportamiento normal. Referencias: [capacidad de CPU en Linux](https://docs.kernel.org/scheduler/sched-capacity.html) y [planificación y afinidad en Android](https://developer.android.com/agi/sys-trace/threads-scheduling).

### Comparación en la A9

1. Mantener la misma salida de auriculares, el mismo sonido y **2048 frames**.
2. Iniciar con **Automático (Android)**, esperar a que termine el arranque y registrar el render reciente, el tiempo del motor y de conversión, los núcleos observados y cuántos xruns se agregan durante 20–30 segundos.
3. Detener, seleccionar **Núcleos de rendimiento** e iniciar de nuevo. Repetir la misma secuencia de notas y la misma duración.
4. Verificar en el panel que la máscara efectiva coincide con la selección solicitada. Si se mantiene en automático, el motivo o `errno` indica qué revisar.
5. Buscar render sostenido por debajo del 100 %, con margen para los picos, y un contador de xruns que deje de crecer. Sólo después ensayar 1024 o 512 frames.

Las cifras de CPU equivalen al tiempo de un núcleo, no al porcentaje de todos los núcleos de la tablet. La cantidad acumulada de xruns al arrancar importa menos que su crecimiento durante la prueba estable.

### Validación de 0.1.2

Las pruebas del conversor usan señales sintéticas sin ROMs y una referencia que calcula cada coeficiente de forma independiente. También comprueban el avance racional, los límites de entrada/salida, la invariancia al dividir el audio en bloques, el arranque, el reinicio y la ruta genérica. La ruta nueva conserva el filtro, pero no se presenta como idéntica byte por byte a la posición flotante acumulada anterior: esa posición puede tener deriva numérica.

Las pruebas del selector de CPU no fijan afinidad en la máquina de CI: comprueban la selección sobre topologías y máscaras sintéticas, incluidas restricciones y datos incompletos. La aceptación de la afinidad, la colocación real, la temperatura y los xruns requieren la tablet física.

Validación local: 64 casos del conversor y 12 escenarios de selección de CPU aprobados; AddressSanitizer y UndefinedBehaviorSanitizer sin errores en las pruebas del conversor. También pasan la verificación del motor sin ROMs, 6.176 casos del generador aleatorio, 80.000 mensajes MIDI concurrentes y los 6 escenarios del parser Java. El APK ARM64 compila con Android Lint sin errores y conserva la alineación ZIP/ELF de 16 KiB.

En un microbenchmark del equipo de desarrollo, 480.000 frames de salida en bloques de 256 tardaron una mediana de 114,7 ms de CPU con el conversor anterior y 28,2 ms con el precalculado, sobre cinco pares de ejecuciones. Es una reducción del 75,4 % **sólo en esa prueba del conversor en el host**; no mide el motor completo ni predice el resultado en la A9. En la comparación con la ruta anterior, la diferencia máxima fue inferior a 1,12 unidades del último bit de PCM16, coherente con los cambios de borde que provocaba la deriva de su posición flotante; la referencia racional independiente conserva la respuesta prevista del filtro.

## Cambios anteriores: 0.1.1

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

El workflow **Android alpha** compila cuando se actualiza la rama `android` y permite una ejecución manual. Antes del APK ejecuta la verificación del motor de upstream sin ROMs, las pruebas del parser MIDI, de la cola concurrente, del selector de CPU y del conversor sinc; también ejecuta Android Lint. En **Actions → Android alpha → ejecución terminada → Artifacts**, descarga `RiMU2000-0.1.2-arm64`, que contiene el APK y el código fuente de ese commit.

La compilación de CI no necesita ROMs y no verifica sonido. Las pruebas con firmware y mediciones de rendimiento se realizan por separado en un dispositivo.

## Licencias

Se conservan las licencias y avisos del motor original en [LICENSE](../LICENSE) y [NOTICE.txt](../NOTICE.txt). Gradle copia ambos archivos al APK bajo `assets/licenses/`; se pueden consultar desde la aplicación. Los archivos de la aplicación Android son una adaptación para este port. Los datos extraídos del hardware no forman parte de este código fuente.
