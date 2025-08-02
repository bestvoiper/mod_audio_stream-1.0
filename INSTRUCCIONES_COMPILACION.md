# Instrucciones de Compilación - mod_audio_stream

## Correcciones Aplicadas

### 1. Problema de Tipo `bool` 
**Solucionado**: Cambiado `bool` por `switch_bool_t` en headers para compatibilidad C/C++.

### 2. Dependencias Flexibles
**Solucionado**: Script de build actualizado para manejar sistemas sin `libfreeswitch-dev`.

### 3. Librería WebSocket Automática
**Solucionado**: CMakeLists.txt detecta automáticamente qué librería WebSocket usar (libwsc o IXWebSocket).

## Pasos de Compilación

### 1. Instalar Dependencias Básicas
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git libssl-dev zlib1g-dev libspeexdsp-dev libevent-dev
```

### 2. Para FreeSWITCH desde Fuente
Si FreeSWITCH fue compilado desde fuente:
```bash
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig:$PKG_CONFIG_PATH
```

### 3. Clonar e Inicializar
```bash
cd /usr/src
git clone https://github.com/amigniter/mod_audio_stream.git
cd mod_audio_stream

# Inicializar submodules - método recomendado
git submodule update --init --recursive

# Si el comando anterior falla, usar método alternativo:
# git submodule init
# git submodule update
```

### 4. Compilar
```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
sudo make install
```

## Script Automatizado (Actualizado)
```bash
sudo bash build-mod-audio-stream.sh
```

## Verificación Post-Instalación

### 1. Verificar Módulo Instalado
```bash
ls -la /usr/local/freeswitch/mod/mod_audio_stream.so
```

### 2. Cargar en FreeSWITCH
```bash
fs_cli -x "load mod_audio_stream"
```

### 3. Verificar Carga
```bash
fs_cli -x "module_exists mod_audio_stream"
```

## Troubleshooting

### Error: "No url found for submodule"
**Problema**: Configuración de submodules incompleta.
**Solución**: 
```bash
# Limpiar submodules existentes
git submodule deinit --all
rm -rf .git/modules/*

# Volver a inicializar
git submodule update --init --recursive

# Si persiste el error, clonar manualmente:
git clone https://github.com/machinezone/IXWebSocket libs/IXWebSocket
# O alternativamente:
git clone https://github.com/amigniter/libwsc libs/libwsc
```

### Error: "libfreeswitch-dev not found"
**Solución**: Normal si FreeSWITCH fue compilado desde fuente. El script continuará automáticamente.

### Error: "bool not defined"
**Solución**: Ya corregido en esta versión.

### Error: "WebSocket library not found"
**Solución**: 
```bash
git submodule update --init --recursive
```

### Error: "FreeSWITCH modules dir not found"
**Solución**: Verificar PKG_CONFIG_PATH:
```bash
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig:$PKG_CONFIG_PATH
pkg-config --exists freeswitch && echo "FreeSWITCH found" || echo "FreeSWITCH not found"
```

## Configuración Recomendada

### En dialplan o script:
```javascript
// Configuración para máxima estabilidad
session.setVariable("STREAM_HEART_BEAT", "30");
session.setVariable("STREAM_BUFFER_SIZE", "40");
session.setVariable("STREAM_SUPPRESS_LOG", "true");
```

### Para debugging:
```javascript
session.setVariable("STREAM_SUPPRESS_LOG", "false");
// Los niveles de debug se configuran en tiempo de ejecución
```

## Testing

### Prueba Básica
```bash
fs_cli -x "uuid_audio_stream \${create_uuid()} start ws://localhost:8080/audio mono 8k"
```

### Prueba de Límites
El módulo ahora soporta hasta 50 canales concurrentes. Para probar:
```bash
# Desde FreeSWITCH CLI
originate {STREAM_HEART_BEAT=30}sofia/internal/1000@localhost echo
# Repetir hasta 50+ llamadas para verificar estabilidad
```

## Logs y Monitoreo

### Logs de Debug
```bash
tail -f /usr/local/freeswitch/log/freeswitch.log | grep "mod_audio_stream"
```

### Verificar Canales Activos
Los logs mostrarán información como:
```
[DEBUG][LEVEL4] Active channels: 5/50
[DEBUG][LEVEL4] Session initialized successfully
```

## Notas Importantes

1. **Compatibilidad**: Esta versión es compatible con FreeSWITCH 1.10+
2. **Memoria**: Cada sesión usa ~2-4MB de RAM
3. **Performance**: Optimizado para baja latencia
4. **Escalabilidad**: Soporta hasta 50 sesiones concurrentes (configurable)

Para soporte adicional, consultar MEJORAS.md.
