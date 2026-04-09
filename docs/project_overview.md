# project overview

## project title

IoT fruit freshness detection system using ESP32-S3, HDC1080, AS7341, and camera module

## short summary

This project is a low-cost IoT fruit freshness detection system developed using an ESP32-S3, HDC1080, AS7341, and camera module, with banana used as the main case study for prototype development and testing. The system combines environmental sensing, spectral reflectance, and RGB imaging to estimate freshness, support spoilage monitoring, and visualize results on a web dashboard.

## system flow

1. ESP32 wakes up
2. HDC1080 reads temperature and humidity
3. AS7341 onboard LED turns on
4. camera captures one image
5. AS7341 reads spectral channels
6. firmware computes banana stage
7. image and JSON are uploaded separately
8. OLED shows status
9. ESP32 enters deep sleep

## maturity outputs used in the sketch

- Green / Unripe
- Proceeding from Unripe to Ripe
- Ripe
- Proceeding from Ripe to Overripe
- Overripe
- Spoiled

## note on temperature and humidity

temperature and humidity are used for storage interpretation only.  
they do not directly override the spectral maturity stage.
