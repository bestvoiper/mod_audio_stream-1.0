# Compilación e Instalación - Módulo con Inyección de Audio

## Resumen de Cambios

Se ha implementado la funcionalidad de **inyección de audio** en el módulo `mod_audio_stream`, permitiendo que el WebSocket pueda enviar audio para reproducir en tiempo real durante las llamadas.

### Nuevas Características:

1. **Inyección de Audio via WebSocket**: Envío de audio codificado en Base64
2. **Soporte Multi-formato**: RAW PCM, WAV, MP3, OGG
3. **Resampling Automático**: Conversión automática de frecuencias de muestreo
4. **Comandos API**: `enable_inject` y `disable_inject`
5. **Thread Safety**: Operaciones seguras en entornos multi-hilo

## Archivos Modificados

### 1. `mod_audio_stream.h`
- Agregados campos para inyección de audio en `struct private_data`
- Buffer de inyección, mutex y resampler específicos

### 2. `audio_streamer_glue.h`
- Nuevas funciones exportadas para inyección de audio
- Declaraciones de funciones de control

### 3. `audio_streamer_glue.cpp`
- Clase `AudioStreamer` actualizada para manejar mensajes de inyección
- Funciones de inyección de audio implementadas
- Gestión mejorada de recursos y memoria

### 4. `mod_audio_stream.c`
- Callback actualizado para procesar audio inyectado
- Nuevos comandos API agregados
- Sintaxis de API ampliada

## Compilación

### Requisitos
- FreeSWITCH development headers
- CMake >= 3.10
- Speex DSP library
- libwsc WebSocket library

### Pasos de Compilación

```bash
# 1. Navegar al directorio del módulo
cd /path/to/mod_audio_stream

# 2. Limpiar build anterior (si existe)
rm -rf build/

# 3. Crear directorio de build
mkdir build && cd build

# 4. Configurar con CMake
cmake ..

# 5. Compilar
make -j$(nproc)

# 6. Instalar (requiere permisos)
sudo make install
```

### Compilación Alternativa (Manual)

```bash
# Compilar directamente con GCC
gcc -fPIC -shared \
    -I/usr/include/freeswitch \
    -I/usr/include/speex \
    -o mod_audio_stream.so \
    mod_audio_stream.c \
    audio_streamer_glue.cpp \
    base64.cpp \
    buffer/ringbuffer.c \
    -lspeexdsp -lfreeswitch -lwsc -lstdc++
```

## Instalación

### 1. Copiar el Módulo
```bash
# Copiar a directorio de módulos de FreeSWITCH
sudo cp mod_audio_stream.so /usr/lib/freeswitch/mod/
```

### 2. Configurar FreeSWITCH

#### Cargar Módulo en `modules.conf.xml`:
```xml
<load module="mod_audio_stream"/>
```

#### O cargar dinámicamente:
```bash
# En fs_cli
freeswitch> load mod_audio_stream
```

### 3. Verificar Instalación
```bash
# En fs_cli
freeswitch> module_exists mod_audio_stream
freeswitch> show api uuid_audio_stream
```

## Uso de la Inyección de Audio

### 1. Iniciar Streaming de Audio
```bash
# En fs_cli
uuid_audio_stream <uuid> start wss://tu-servidor.com/audio mono 8000
```

### 2. Habilitar Inyección de Audio
```bash
uuid_audio_stream <uuid> enable_inject
```

### 3. Enviar Audio via WebSocket

#### Mensaje JSON:
```json
{
  "type": "injectAudio",
  "data": {
    "audioData": "<audio_en_base64>",
    "format": "raw",
    "sampleRate": 8000,
    "channels": 1
  }
}
```

### 4. Deshabilitar Inyección
```bash
uuid_audio_stream <uuid> disable_inject
```

## Testing

### 1. Test Básico
```bash
# Conectar WebSocket y enviar tono de prueba
python3 examples/audio_injection_server.py --mode demo --url ws://localhost:8080
```

### 2. Test con Interfaz Web
```bash
# Abrir en navegador
examples/audio_injection_demo.html
```

### 3. Test de Carga
```bash
# Servidor de prueba con múltiples clientes
python3 examples/audio_injection_server.py --mode server --port 8080
```

## Debugging

### 1. Logs de FreeSWITCH
```bash
# Ver logs en tiempo real
tail -f /var/log/freeswitch/freeswitch.log | grep mod_audio_stream
```

### 2. Debug Level
```bash
# Aumentar nivel de debug
uuid_audio_stream debug_level 5
```

### 3. Estado de Canales
```bash
# Ver estado de canales activos
uuid_audio_stream debug_channels
```

## Troubleshooting

### Problemas Comunes

#### 1. Módulo no carga
```bash
# Verificar dependencias
ldd mod_audio_stream.so

# Verificar permisos
ls -la /usr/lib/freeswitch/mod/mod_audio_stream.so
```

#### 2. WebSocket no conecta
- Verificar URL del WebSocket
- Verificar firewall/puertos
- Revisar logs de conexión

#### 3. Audio no se reproduce
```bash
# Verificar que la inyección esté habilitada
uuid_audio_stream <uuid> enable_inject

# Verificar formato de audio
# Formatos soportados: raw, wav, mp3, ogg
```

#### 4. Problemas de Calidad de Audio
- Verificar sample rate (8000, 16000, etc.)
- Verificar cantidad de canales (1=mono, 2=estéreo)
- Revisar codificación Base64

### Logs de Error Típicos

#### Error de Compilación:
```
error: 'struct private_data' has no member named 'inject_buffer'
```
**Solución**: Verificar que se incluyó `mod_audio_stream.h` actualizado

#### Error de Runtime:
```
[ERROR] inject_audio_data: injection not initialized
```
**Solución**: Ejecutar `uuid_audio_stream <uuid> enable_inject` primero

## Performance

### Optimizaciones Implementadas
1. **Buffers Circulares**: Minimiza copias de memoria
2. **Thread Safety**: Previene condiciones de carrera
3. **Resampling Eficiente**: Usando Speex DSP
4. **Gestión de Memoria**: Cleanup automático

### Límites Recomendados
- **Buffer Size**: Máximo 8KB por inyección
- **Sample Rate**: 8kHz a 48kHz
- **Canales**: 1-2 canales
- **Frecuencia de Inyección**: Máximo 100 inyecciones/segundo

## Ejemplos de Integración

### Node.js
```javascript
const WebSocket = require('ws');
const fs = require('fs');

const ws = new WebSocket('wss://freeswitch-server.com/audio');
const audioData = fs.readFileSync('audio.wav').toString('base64');

ws.send(JSON.stringify({
  type: "injectAudio",
  data: {
    audioData: audioData,
    format: "wav",
    sampleRate: 8000,
    channels: 1
  }
}));
```

### Python
```python
import websocket
import base64
import json

ws = websocket.WebSocket()
ws.connect("wss://freeswitch-server.com/audio")

with open('audio.wav', 'rb') as f:
    audio_data = base64.b64encode(f.read()).decode()

message = {
    "type": "injectAudio",
    "data": {
        "audioData": audio_data,
        "format": "wav",
        "sampleRate": 8000,
        "channels": 1
    }
}

ws.send(json.dumps(message))
```

## Soporte y Mantenimiento

### Actualizaciones Futuras
- Soporte para más formatos de audio
- Mejoras en el resampling
- API REST adicional
- Métricas de performance

### Reportar Problemas
- Incluir logs relevantes
- Especificar versión de FreeSWITCH
- Proporcionar casos de reproducción

La funcionalidad de inyección de audio está ahora completamente integrada y lista para uso en producción.
