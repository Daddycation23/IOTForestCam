/**
 * @file main.cpp
 * @brief IOT Forest Cam - Full StorageReader Test
 * LILYGO T3-S3 V1.2
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "StorageReader.h"

// LILYGO T3-S3 V1.2 OLED pins
#define OLED_SDA 18
#define OLED_SCL 17

Adafruit_SSD1306 display(128, 64, &Wire, -1);
StorageReader storage;

void setup() {
    Serial.begin(115200);
    
    // Init OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.println("T3-S3 V1.2");
    display.println();
    display.println("Mounting SD...");
    display.display();
    
    // Init StorageReader
    if (!storage.begin()) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("SD Mount Failed!");
        display.println();
        display.println("Check /images/");
        display.println("folder exists with");
        display.println(".jpg files");
        display.display();
        return;
    }
    
    // Show image count
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("SD Card OK!");
    display.printf("Images: %d\n", storage.imageCount());
    display.display();
    delay(2000);
    
    // If images found, show first image info
    if (storage.imageCount() > 0) {
        ImageInfo info;
        if (storage.getImageInfo(0, info)) {
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("First Image:");
            display.println(info.filename);
            display.printf("Size: %lu bytes\n", info.fileSize);
            display.printf("Blocks: %u\n", info.totalBlocks);
            display.println();
            display.println("Block size: 512B");
            display.println("(CoAP ready)");
            display.display();
            
            Serial.printf("\n=== Image: %s ===\n", info.filename);
            Serial.printf("Size: %lu bytes\n", info.fileSize);
            Serial.printf("Total 512B blocks: %u\n", info.totalBlocks);
        }
    } else {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("No images found!");
        display.println();
        display.println("Add .jpg files to:");
        display.println("/images/ folder");
        display.display();
    }
}

void loop() {
    delay(1000);
}