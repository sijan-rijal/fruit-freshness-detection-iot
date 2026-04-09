# Banana Freshness IoT Project

A low-cost IoT fruit freshness detection system developed using an ESP32-S3, HDC1080, AS7341, and camera module, with banana used as the main case study for prototype development and testing. The system combines environmental sensing, spectral reflectance, and RGB imaging to estimate freshness, support spoilage monitoring, and visualize results on a web dashboard.

## what is inside

- `firmware/banana_monitor_final/`  
  ESP32 sketch for sensor reading, maturity classification, image capture, upload, OLED display, and deep sleep.

- `dashboard/`  
  Flask dashboard to receive JSON + JPEG uploads and show the latest reading on a web page.

- `docs/`  
  simple project notes, classification summary, and documentation files for submission work.

- `poster/`  
  for poster design / poster app files you will add later.


## quick setup

### firmware side
1. open `firmware/banana_monitor_final/banana_monitor_final.ino`
2. put your own Wi-Fi name and password there
3. make sure `camera_pins.h` exists for your board
4. upload to ESP32-S3

### dashboard side
1. go to `dashboard/`
2. create virtual env if you want
3. install:
   ```bash
   pip install -r requirements.txt
   ```
4. run:
   ```bash
   python app.py
   ```
5. open browser on:
   ```text
   http://127.0.0.1:5002
   ```

## notes
- live data created by dashboard is stored in `dashboard/data_store/`
