#!/bin/bash
### Initial one liner:
# sudo apt-get -y install git \
#    && cd /usr/src/ \
#    && git clone https://github.com/amigniter/mod_audio_stream.git \
#    && cd mod_audio_stream \
#    && sudo bash ./build-mod-audio-stream.sh

echo "Installing dependencies..."

# Try to install libfreeswitch-dev, but continue if it fails
apt-get update
apt-get -y install libssl-dev zlib1g-dev libspeexdsp-dev libevent-dev

# Try libfreeswitch-dev, but don't fail if it's not available
if apt-get -y install libfreeswitch-dev 2>/dev/null; then
    echo "libfreeswitch-dev installed successfully"
else
    echo "Warning: libfreeswitch-dev not available as package"
    echo "Assuming FreeSWITCH is installed from source"
fi

echo "Initializing submodules..."
# Try the recommended method first
if git submodule update --init --recursive; then
    echo "Submodules initialized successfully"
else
    echo "Standard submodule init failed, trying alternative method..."
    
    # If that fails, try manual initialization
    git submodule deinit --all 2>/dev/null || true
    rm -rf .git/modules/* 2>/dev/null || true
    
    if git submodule update --init --recursive; then
        echo "Submodules initialized with alternative method"
    else
        echo "Submodule initialization failed, cloning libraries manually..."
        
        # Manual clone as fallback
        if [ ! -d "libs/IXWebSocket/.git" ]; then
            echo "Cloning IXWebSocket..."
            rm -rf libs/IXWebSocket
            git clone https://github.com/machinezone/IXWebSocket libs/IXWebSocket
        fi
        
        if [ ! -d "libs/libwsc/.git" ]; then
            echo "Cloning libwsc..."
            rm -rf libs/libwsc
            git clone https://github.com/amigniter/libwsc libs/libwsc
        fi
    fi
fi

# Verify at least one WebSocket library is available
if [ ! -f "libs/IXWebSocket/CMakeLists.txt" ] && [ ! -f "libs/libwsc/CMakeLists.txt" ]; then
    echo "ERROR: No WebSocket library found!"
    echo "Please check your internet connection and try again."
    exit 1
fi

# Set PKG_CONFIG_PATH for FreeSWITCH
FS_PKGCONFIG=/usr/local/freeswitch/lib/pkgconfig
if [ -d "$FS_PKGCONFIG" ]; then
    echo "Using FreeSWITCH from: $FS_PKGCONFIG"
    export PKG_CONFIG_PATH=$FS_PKGCONFIG:$PKG_CONFIG_PATH
else
    echo "FreeSWITCH pkg-config not found in $FS_PKGCONFIG"
    echo "Will use system-wide FreeSWITCH installation"
fi

echo "Building module..."
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
echo "Installing module..."
make install

echo "Build completed successfully!"
