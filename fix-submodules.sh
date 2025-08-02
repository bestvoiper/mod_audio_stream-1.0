#!/bin/bash
# fix-submodules.sh - Script para resolver problemas de submodules

echo "=== mod_audio_stream Submodule Fix Script ==="
echo

# Function to check if a directory exists and has content
check_lib() {
    local lib_path="$1"
    local lib_name="$2"
    
    if [ -f "$lib_path/CMakeLists.txt" ]; then
        echo "✓ $lib_name is properly installed"
        return 0
    else
        echo "✗ $lib_name is missing or incomplete"
        return 1
    fi
}

# Check current status
echo "Checking current library status..."
ixwebsocket_ok=false
libwsc_ok=false

if check_lib "libs/IXWebSocket" "IXWebSocket"; then
    ixwebsocket_ok=true
fi

if check_lib "libs/libwsc" "libwsc"; then
    libwsc_ok=true
fi

if [ "$ixwebsocket_ok" = true ] || [ "$libwsc_ok" = true ]; then
    echo
    echo "✓ At least one WebSocket library is available. Build should work."
    exit 0
fi

echo
echo "No WebSocket libraries found. Attempting to fix..."

# Clean up existing submodules
echo "1. Cleaning existing submodule configuration..."
git submodule deinit --all 2>/dev/null || true
rm -rf .git/modules/* 2>/dev/null || true

# Try standard git submodule approach
echo "2. Attempting standard submodule initialization..."
if git submodule update --init --recursive; then
    echo "✓ Standard submodule initialization successful"
    exit 0
fi

# If that fails, try manual cloning
echo "3. Standard method failed. Cloning libraries manually..."

# Create libs directory if it doesn't exist
mkdir -p libs

# Clone IXWebSocket
if [ ! -f "libs/IXWebSocket/CMakeLists.txt" ]; then
    echo "   Cloning IXWebSocket..."
    rm -rf libs/IXWebSocket
    if git clone https://github.com/machinezone/IXWebSocket libs/IXWebSocket; then
        echo "   ✓ IXWebSocket cloned successfully"
    else
        echo "   ✗ Failed to clone IXWebSocket"
    fi
fi

# Clone libwsc
if [ ! -f "libs/libwsc/CMakeLists.txt" ]; then
    echo "   Cloning libwsc..."
    rm -rf libs/libwsc
    if git clone https://github.com/amigniter/libwsc libs/libwsc; then
        echo "   ✓ libwsc cloned successfully"
    else
        echo "   ✗ Failed to clone libwsc"
    fi
fi

echo
echo "4. Final verification..."

# Final check
ixwebsocket_ok=false
libwsc_ok=false

if check_lib "libs/IXWebSocket" "IXWebSocket"; then
    ixwebsocket_ok=true
fi

if check_lib "libs/libwsc" "libwsc"; then
    libwsc_ok=true
fi

if [ "$ixwebsocket_ok" = true ] || [ "$libwsc_ok" = true ]; then
    echo
    echo "✓ SUCCESS! WebSocket libraries are now available."
    echo "You can now run: mkdir build && cd build && cmake .. && make"
else
    echo
    echo "✗ FAILED! Could not install WebSocket libraries."
    echo "Please check your internet connection and try again."
    echo "You may need to clone the libraries manually."
    exit 1
fi
