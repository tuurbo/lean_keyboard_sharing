#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "Building Linux Sender for Keyboard Switcher..."
g++ -O3 -o sender_linux sender_linux.cpp -lpthread

echo
echo "Build successful: sender_linux"
echo
echo "You need permission to read /dev/input/* devices."
echo "Run as root or add your user to the input group:"
echo "  sudo usermod -a -G input \$USER"
echo "Then log out and back in."
echo
echo "Usage: ./sender_linux <receiver_ip> [--hotkey=ALT+1]"
