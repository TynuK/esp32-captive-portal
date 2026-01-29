# 🚀 ESP32 Captive Portal Library

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A comprehensive captive portal library for ESP32 that automatically detects and redirects iOS, Android, and Windows devices to a configuration web interface.

## 📦 Installation

### ESP-IDF
```bash
# Create components directory if it doesn't exist
mkdir -p components

# Clone the repository
cd components
git clone https://github.com/TynuK/esp32-captive-portal.git
cd ..
⚙️ Mandatory ESP-IDF Configuration
Important: Before using the library, you must increase the HTTP header length limit in ESP-IDF configuration:

bash
cd your-esp32-project
idf.py menuconfig
Navigate to:

text
Component config → HTTP Server → Max HTTP Request Header Length → Set to 1024 (or 2048 for better compatibility)
This is required because iOS and Android devices send longer HTTP headers that exceed the default ESP-IDF limit.

PlatformIO
ini
lib_deps = 
    https://github.com/TynuK/esp32-captive-portal.git