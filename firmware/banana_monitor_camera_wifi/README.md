# banana monitor firmware

this folder keeps the final ESP32 sketch used for the banana case study.

## files

- `banana_monitor_final.ino`  
- `camera_pins.h`  


## before upload
make sure to updated wifi details in part 1 - hardware & network configuration

local server
const NetworkConfig NET_CFG = {
  WIFI_SSID,
  WIFI_PASSWORD,
  "http://192.168.178.90:5002/api/reading",
  "http://192.168.178.90:5002/api/upload_image",
  "banana-monitor-01"
};


