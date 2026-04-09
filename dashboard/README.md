# dashboard

this is the Flask web dashboard for the project.

it does 3 main things:

1. receives JSON reading from ESP32
2. receives JPEG image from ESP32
3. shows the latest result in browser

## run local

```bash
pip install -r requirements.txt
python app.py
```

then open:

```text
http://127.0.0.1:5002
```

## files

- `app.py` main dashboard server
- `requirements.txt` python packages
- `wsgi.py` helpful for deployment
- `data_store/` latest files and log data
