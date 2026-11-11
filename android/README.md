# RiMU2000 para Android — 0.1.4 alpha

Primera aplicación independiente para Android del motor de [S-MU2000](https://github.com/tarboh/S-MU2000). Ejecuta el firmware del MU2000 y conserva la emulación del SH7043 y de los dos SWP30, incluidos los compiladores JIT ARM64 de upstream. La interfaz Android conecta ese motor con MIDI y audio del dispositivo.

La alpha está orientada a la Samsung Galaxy Tab A9 y requiere Android 8.0 o superior con CPU ARM64. El rendimiento y la latencia deben medirse en la tablet: que el código compile no garantiza ausencia de cortes con todas las canciones o sonidos.

## Cambios de 0.1.4

Esta versión reorganiza la pantalla para elegir y tocar sonidos desde la tablet. Conserva el motor, la síntesis del firmware y los ajustes de audio de 0.1.3.

- **Explorador de sonidos.** La lista usa los nombres y mapas reconocidos de la ROM importada y del modo actual del firmware. Permite buscar por nombre, banco o programa, filtrar por familia y banco, y guardar favoritos locales. Buscar, filtrar y marcar una estrella no envía MIDI. Tocar una fila solicita ese sonido para el destino que indica la pantalla.
- **Destino explícito.** El teclado, la selección de sonido y los seis mandos se dirigen al **puerto A y al canal MIDI elegido**. Una parte del MU2000 puede recibir en otro canal o compartirlo con otras partes: estos controles afectan a todas las partes que escuchan esa ruta. El resto de los canales sigue disponible para la interpretación multitímbrica desde MIDI externo. El seguimiento del último Program Change actualiza el nombre mostrado, sin mover el destino de los controles.
- **Confirmación del firmware.** Una selección queda pendiente hasta que un snapshot posterior refleja banco y programa. El nombre actual procede de la voz que realmente seleccionó el firmware; no se sustituye por el nombre solicitado del catálogo. El programa se presenta como 1–128 y se envía como 0–127. Un cambio de modo o de ROM invalida el catálogo anterior.
- **Mandos de sonido.** Cutoff, resonancia, attack y release envían CC74, CC71, CC73 y CC72. Son desplazamientos relativos a la voz, con centro MIDI 64, no valores físicos de frecuencia o duración. Reverb y chorus envían CC91 y CC93 y representan el envío de cada parte al efecto. Todos usan valores MIDI 0–127 leídos de RAM: no se envían valores iniciales al abrir la pantalla, cambiar de canal o refrescar. Los controles sólo envían al recibir un gesto del usuario. Si varias partes comparten ruta y sus valores difieren, la interfaz indica el estado mixto.
- **Conexiones y diagnóstico.** El menú **⋮** abre los ajustes de ROMs, audio y MIDI, las medidas técnicas, la selección manual de banco/programa y las licencias. El botón **Teclas** muestra u oculta el teclado en pantalla, inicialmente oculto. Se conservan el detalle de cortes y la copia de diagnóstico.

El catálogo no pretende cubrir rutas que upstream todavía no identifica. Omite el banco especial XG MSB 16/LSB 0–1; en modo 0 muestra únicamente las voces melódicas reconocidas y no deduce sus baterías a partir de las tablas XG. El modo 0 se presenta como GM y señala su alcance melódico. Si el mapa de la ROM no se reconoce, no inventa un catálogo GM. Las categorías agrupan los programas para facilitar la búsqueda; el nombre de cada entrada procede del mapa de voces reconocido.

En la comprobación con firmware EX 2.01, modo XG y set 1, el catálogo contiene **1.426 entradas seleccionables**: 1.386 melódicas, 32 direcciones de kits de batería y 8 de kits SFX. No significa 1.426 timbres distintos: la deduplicación melódica conserva programa y registro, y los kits conservan sus direcciones alternativas. En modo 0 se obtuvieron 472 entradas melódicas y se omitieron los kits. La cantidad depende del mapa, el modo y el set del firmware.

Los valores de los mandos pueden cambiar después de seleccionar una voz, porque el firmware repone algunos ajustes propios del sonido. Por eso reverb 40, chorus 0 y el centro 64 son referencias iniciales, no una afirmación sobre el sonido que está activo. La aplicación vuelve a leer el estado efectivo.

### Revisión visual y límites

La revisión de composición usa las vistas Android de la aplicación con [Robolectric Native Graphics](https://developer.android.com/training/testing/ui-tests/screenshot) y datos sintéticos identificados como QA, sin cargar ROMs ni ejecutar audio. El harness vive fuera del proyecto distribuido y sus dependencias no se añaden al APK. Estas capturas ayudan a revisar tamaños, orientación y texto grande; no verifican AAudio, MIDI físico, rendimiento ni el comportamiento exacto de las barras del sistema de la tablet.

## Cambios anteriores: 0.1.3

Esta actualización responde a dos resultados distintos en la A9: con 1024 frames aparecen xruns incluso con sonidos sencillos; con 2048 la reproducción suele ser estable y los cortes aparecen en algunos pads con sustain. La captura de 0.1.2 con afinidad aplicada mostró render reciente del 54,3 %, frente al 127,8 % anterior, pero esas capturas no son un ensayo controlado de todos los sonidos.

- **Callback independiente del buffer.** Se deja `AAudioStreamBuilder_setFramesPerDataCallback` sin tamaño fijo. Android elige los bloques que solicita y la app conserva el buffer seleccionado: 512, 1024 o 2048 frames, redondeado a ráfagas y con un mínimo de dos ráfagas. El panel separa el callback observado del buffer solicitado y del real. No se duplica el buffer de forma oculta. Esto permite entregar fragmentos antes; no garantiza que 1024 funcione en todos los sonidos.
- **Nombre del sonido real.** El panel superior lee las partes de la RAM del firmware y el registro de voz que éste seleccionó. Muestra nombre, programa 1–128, banco MSB/LSB, parte y ruta de recepción. Por defecto sigue la ruta del último Program Change completo, incluidos mensajes fragmentados y running status. No envía MIDI al refrescar, ni cambia el canal del teclado en pantalla, sus campos de edición o sustain. Si varias partes reciben en esa ruta, aparecen todas.
- La lectura de nombres utiliza el mapa EX 2.01 que ya emplea el escritorio de upstream. Si la ROM o una voz no tienen un nombre reconocido, se indica **Nombre no disponible**. No se sustituye por un catálogo GM supuesto. Los nombres se resuelven fuera del hilo de audio y no se distribuyen datos de ROM en el APK.
- **Registro de cortes.** Se conservan los ocho bloques con mayor tiempo de trabajo relativo a su duración de audio, y por separado los cuatro últimos bloques en los que se observó un incremento del contador de xruns. Incluye tiempo de reloj/CPU, intervalo entre callbacks, núcleos inicial/final, bytes MIDI consumidos, intentos de compilación JIT, voces observadas y una parte identificada en ese instante. El historial usa memoria preasignada y datos atómicos; el callback no formatea texto, escribe archivos ni adquiere bloqueos para publicarlo.
- **Detalle de cortes**, desactivado inicialmente, inicia una captura nueva y añade dos lecturas `getrusage(RUSAGE_THREAD)` por callback para obtener fallos de página y cambios de contexto. Apagar y volver a encender inicia otra captura; apagarlo solo conserva lo registrado y continúa con las medidas básicas. **Copiar diagnóstico** copia al portapapeles local el estado y los registros disponibles, incluso si la sección está cerrada.
- Se muestran las voces activas de ambos SWP30 y los intentos recientes de compilación SH2/MEG, además de sus fallos acumulados. Las voces incluyen las colas de release y se muestrean entre fragmentos de hasta 256 frames: son máximos observados, no una cuenta exacta de teclas ni de picos entre muestras.

La separación de callback y buffer sigue la [recomendación de Oboe para aplicaciones que aceptan bloques variables](https://github.com/google/oboe/blob/main/include/oboe/AudioStreamBuilder.h). Fijar un callback grande agrupa datos; no garantiza su periodicidad. La [terminología de buffers de Oboe](https://github.com/google/oboe/wiki/TechNote_BufferTerminology) distingue capacidad máxima, tamaño utilizable y ráfaga del dispositivo.

### Cómo interpretar y comparar

El porcentaje compara trabajo medido con la duración de audio del callback. Un callback corto puede superar el 100 % y ser absorbido por la reserva del buffer sin causar un xrun. A la inversa, puede observarse un xrun con trabajo inferior al 100 % si la entrega llegó tarde. El contador de xruns puede actualizarse después del corte: una fila no identifica automáticamente su causa. El intervalo entre inicios no es una medida de latencia de tecla a sonido.

Al cambiar el tamaño del callback también cambia el intervalo sobre el que se mide cada máximo. El máximo de 0.1.3 no debe compararse directamente con el máximo de los bloques de 2048 de 0.1.2; comparar además xruns y carga media para la misma secuencia de notas.

El tiempo de CPU y reloj, los fallos de página, la migración observada y la compilación permiten contrastar hipótesis. Un fallo de página no identifica por sí solo las ROMs, un mismo núcleo al inicio y al final no descarta migraciones intermedias, y consumir MIDI en un bloque no demuestra que ese MIDI causara el pico. La parte guardada en el historial es la primera que recibe en la ruta observada; otras partes también pueden estar aportando carga.

Se conserva la síntesis del firmware, ambos SWP30 y el sinc de 64 coeficientes de 0.1.2. No se activan los modos experimentales `native_engine`/`native_fx`. Tampoco se habilitan temporizadores por muestra del perfil interno: añaden trabajo y, en ejecución serial, algunos contadores agrupan los dos chips.

1. Mantener auriculares y **Núcleos de rendimiento**. Empezar con 2048 y comprobar la máscara efectiva.
2. Activar **Detalle de cortes** y repetir el pad con sustain que produce el problema. Copiar el diagnóstico antes de cambiar de prueba; ahora incluye su nombre y banco.
3. Detener, elegir 1024 e iniciar. Hacer una captura breve sin tocar y otra con notas/acordes, reiniciando el detalle entre ellas. El motor sigue emulando aun en silencio; un corte en reposo no demuestra por sí solo un problema del planificador.
4. Comparar xruns, carga, voces, JIT y los registros de esos incrementos. No extrapolar porcentajes medidos en x86 como si fueran mediciones de la A9.

### Validación de 0.1.3

Las pruebas nuevas usan datos sintéticos: comprueban el mapa de las 64 partes (incluido el orden especial del canal 10), nombres por registro real, bancos/recepción/baterías, límites de memoria y lecturas concurrentes; el seguimiento de Program Change; y la retención y publicación concurrente de los bloques más lentos e incrementos de xruns. Las pruebas del conversor comprueban además que cambiar la división de los bloques conserva su salida. La compilación y Android Lint se ejecutan para ARM64. AAudio, el controlador MIDI y el rendimiento de esta versión requieren la tablet física.

## Cambios anteriores: 0.1.2

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

1. Instala el APK ARM64 y abre **⋮ → Conexiones** para importar las ROMs.
2. Conecta el controlador USB-MIDI, selecciónalo en Conexiones y elige la salida de auriculares o altavoz de la tablet.
3. Empieza con un buffer de **2048 frames**, inicia el audio y prueba unas notas. Si funciona estable, prueba 1024 y después 512.
4. Elige el canal de los controles, busca un sonido y toca su fila para solicitarlo. El LCD puede estar siguiendo un Program Change externo de otro canal; el destino del buscador y los mandos sigue indicado por separado. Para tocar sin controlador, abre **Teclas**.
5. En **⋮ → Diagnóstico**, anota el render, los callbacks fuera de presupuesto y los xruns. Son medidas distintas; el porcentaje de render no equivale a la latencia entre tecla y sonido.

El tamaño elegido solicita el buffer utilizable. Android elige el tamaño del callback por separado; el diagnóstico muestra ambos valores efectivos. A 48 kHz, 1024 frames representan 21,33 ms de audio y 2048 representan 42,67 ms; esos valores no son una medición de la latencia total entre tecla y sonido.

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

El workflow **Android alpha** compila cuando se actualiza la rama `android` y permite una ejecución manual. Antes del APK ejecuta la verificación del motor de upstream sin ROMs, las pruebas de MIDI, cola concurrente, selector de CPU, conversor sinc, nombres de partes, catálogo de voces y registro de callbacks; también ejecuta Android Lint. En **Actions → Android alpha → ejecución terminada → Artifacts**, descarga `RiMU2000-0.1.4-arm64`, que contiene el APK y el código fuente de ese commit.

La compilación de CI no necesita ROMs y no verifica sonido. Las pruebas con firmware y mediciones de rendimiento se realizan por separado en un dispositivo.

## Licencias

Se conservan las licencias y avisos del motor original en [LICENSE](../LICENSE) y [NOTICE.txt](../NOTICE.txt). Gradle copia ambos archivos al APK bajo `assets/licenses/`; se pueden consultar desde la aplicación. Los archivos de la aplicación Android son una adaptación para este port. Los datos extraídos del hardware no forman parte de este código fuente.
