from __future__ import annotations

import json
import math
import os
import tempfile
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List

from flask import Flask, jsonify, render_template_string, request, send_file

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 10 * 1024 * 1024  # 10 MB max upload

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data_store"
DATA_DIR.mkdir(exist_ok=True)

LATEST_JSON_PATH = DATA_DIR / "latest_reading.json"
LOG_JSONL_PATH = DATA_DIR / "readings_log.jsonl"
LATEST_IMAGE_PATH = DATA_DIR / "latest_upload.jpg"
LATEST_IMAGE_META_PATH = DATA_DIR / "latest_image_meta.json"

STATE_LOCK = threading.Lock()


# BAND METADATA
BAND_META: Dict[str, Dict[str, Any]] = {
    "F1": {"nm": 415, "title": "F1", "label": "Violet", "meaning": "Violet reflection", "color": "#7c3aed"},
    "F2": {"nm": 445, "title": "F2", "label": "Indigo / Blue", "meaning": "Blue-indigo reflection", "color": "#4f46e5"},
    "F3": {"nm": 480, "title": "F3", "label": "Blue", "meaning": "Blue reflection", "color": "#2563eb"},
    "F4": {"nm": 515, "title": "F4", "label": "Cyan / Green", "meaning": "Cyan/green edge", "color": "#06b6d4"},
    "F5": {"nm": 555, "title": "F5", "label": "Green", "meaning": "Green region", "color": "#16a34a"},
    "F6": {"nm": 590, "title": "F6", "label": "Yellow / Amber", "meaning": "Yellow/amber region", "color": "#eab308"},
    "F7": {"nm": 630, "title": "F7", "label": "Orange / Red", "meaning": "Orange/red region", "color": "#f97316"},
    "F8": {"nm": 680, "title": "F8", "label": "Deep Red", "meaning": "Deep-red region", "color": "#ef4444"},
    "Clear": {"nm": None, "title": "Clear", "label": "Visible Brightness", "meaning": "Overall visible brightness", "color": "#94a3b8"},
    "NIR": {"nm": 910, "title": "NIR", "label": "Near Infrared", "meaning": "Near-infrared reflection", "color": "#db2777"},
}
BAND_ORDER = ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "Clear", "NIR"]
VISIBLE_ORDER = ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8"]



# SMALL HELPERS
def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def atomic_write_text(path: Path, text: str, encoding: str = "utf-8") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding=encoding, dir=str(path.parent), delete=False) as tmp:
        tmp.write(text)
        tmp.flush()
        os.fsync(tmp.fileno())
        tmp_path = Path(tmp.name)
    os.replace(tmp_path, path)


def atomic_write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="wb", dir=str(path.parent), delete=False) as tmp:
        tmp.write(data)
        tmp.flush()
        os.fsync(tmp.fileno())
        tmp_path = Path(tmp.name)
    os.replace(tmp_path, path)


def read_json_file(path: Path) -> Dict[str, Any] | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def append_jsonl(path: Path, obj: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(obj, ensure_ascii=False, allow_nan=False) + "\n")


def make_json_safe(obj: Any) -> Any:
    if isinstance(obj, float):
        if not math.isfinite(obj):
            return None
        return obj

    if isinstance(obj, dict):
        return {str(k): make_json_safe(v) for k, v in obj.items()}

    if isinstance(obj, list):
        return [make_json_safe(v) for v in obj]

    if isinstance(obj, tuple):
        return [make_json_safe(v) for v in obj]

    return obj


def signal_level_label(value: int) -> str:
    if value == 0:
        return "No signal"
    if value <= 500:
        return "Extremely low"
    if value <= 2_000:
        return "Low"
    if value <= 8_000:
        return "Moderate"
    if value <= 20_000:
        return "Strong"
    if value <= 50_000:
        return "Very strong"
    if value <= 62_000:
        return "Near saturation"
    return "Saturated"


def safe_int(value: Any, default: int = 0) -> int:
    try:
        if value is None or value == "":
            return default
        return int(float(value))
    except Exception:
        return default


def safe_float(value: Any, default: float | None = 0.0) -> float | None:
    try:
        if value is None or value == "":
            return default
        f = float(value)
        if not math.isfinite(f):
            return default
        return f
    except Exception:
        return default


def stage_key(stage: str) -> str:
    s = (stage or "").strip().lower()

    # exact transition stages first
    if "proceeding from unripe to ripe" in s:
        return "unripe_to_ripe"
    if "proceeding from ripe to overripe" in s:
        return "ripe_to_overripe"

    if "spoiled" in s or "spoil" in s:
        return "spoiled"
    if "overripe" in s:
        return "overripe"
    if s == "ripe" or ("ripe" in s and "overripe" not in s and "unripe" not in s):
        return "ripe"
    if "green" in s or "unripe" in s:
        return "green"
    if "try again" in s:
        return "try_again"
    if "weak reading" in s:
        return "weak_reading"
    if "overexposed reading" in s:
        return "overexposed"
    if "no data" in s:
        return "no_data"
    return "unknown"


def storage_key(storage: str) -> str:
    s = (storage or "").strip().lower()
    if s == "good":
        return "good"
    if s == "warm":
        return "warm"
    if s == "cold":
        return "cold"
    if s == "dry":
        return "dry"
    if s == "humid":
        return "humid"
    if s == "poor":
        return "poor"
    if s == "unknown":
        return "unknown"
    return "unknown"


def build_band_details(readings: Dict[str, Any]) -> List[Dict[str, Any]]:
    max_value = max([safe_int(readings.get(k, 0)) for k in BAND_ORDER] + [1])
    details: List[Dict[str, Any]] = []

    for key in BAND_ORDER:
        value = safe_int(readings.get(key, 0))
        meta = BAND_META[key]
        details.append(
            {
                "key": key,
                "title": meta["title"],
                "nm": meta["nm"],
                "label": meta["label"],
                "meaning": meta["meaning"],
                "value": value,
                "level": signal_level_label(value),
                "color": meta["color"],
                "relative_percent_of_peak": round((value / max_value) * 100, 1),
            }
        )
    return details


def strongest_band(details: List[Dict[str, Any]], *, visible_only: bool = False) -> Dict[str, Any]:
    candidates = details if not visible_only else [d for d in details if d["key"] in VISIBLE_ORDER]
    if not candidates:
        return {
            "key": "None",
            "value": 0,
            "title": "None",
            "label": "None",
            "meaning": "No data",
            "nm": None,
            "level": "No signal",
            "relative_percent_of_peak": 0.0,
            "color": "#94a3b8",
        }
    return max(candidates, key=lambda d: d["value"])


def top_bands(details: List[Dict[str, Any]], count: int = 3, *, visible_only: bool = False) -> List[Dict[str, Any]]:
    candidates = details if not visible_only else [d for d in details if d["key"] in VISIBLE_ORDER]
    return sorted(candidates, key=lambda d: d["value"], reverse=True)[:count]


def build_story(reading: Dict[str, Any], strongest_visible: Dict[str, Any]) -> str:
    raw_interpretation = reading.get("interpretation") or {}
    fallback = str(raw_interpretation.get("spectral_story") or "").strip()
    readings = reading.get("readings") or {}
    ratios = reading.get("ratios") or {}

    clear_val = safe_int(readings.get("Clear", 0))
    f5 = safe_int(readings.get("F5", 0))
    f7 = safe_int(readings.get("F7", 0))
    f8 = safe_int(readings.get("F8", 0))
    f6 = safe_int(readings.get("F6", 0))
    nir_clear = safe_float(ratios.get("nir_to_clear", 0.0), 0.0) or 0.0

    if fallback:
        return fallback

    # to match thresholds from sketch file more closely
    if clear_val == 0:
        return "No spectral data received yet."
    if clear_val < 600:
        return "Too dark to interpret well."
    if clear_val >= 62000:
        return "Too bright or saturated. Reduce LED current, gain, or distance."
    if strongest_visible["key"] == "F5" and f5 > f7:
        return "Green-side reflection stronger than red side."
    if strongest_visible["key"] == "F7" and (f7 > f5 * 1.15 or f8 > f6):
        return "Red and late-stage bands are rising."
    if nir_clear > 0.25:
        return "NIR is relatively stronger compared with overall brightness."
    return "Mixed spectral balance."


def build_storage_guidance(
    stage: str,
    storage_label: str,
    storage_advice: str,
    eat_recommendation: str,
    final_recommendation: str,
    temperature_c: float | None,
    humidity_rh: float | None,
) -> Dict[str, Any]:
    current_stage_key = stage_key(stage)
    current_storage_key = storage_key(storage_label)

    temp_state = "Unknown"
    humidity_state = "Unknown"

    # exact sketch thresholds
    if temperature_c is not None:
        if temperature_c > 20:
            temp_state = "Warm"
        elif temperature_c < 13:
            temp_state = "Cold"
        else:
            temp_state = "Acceptable"

    if humidity_rh is not None:
        if humidity_rh < 85:
            humidity_state = "Dry"
        elif humidity_rh > 95:
            humidity_state = "Humid"
        else:
            humidity_state = "Acceptable"

    current_summary = storage_advice or "No storage guidance available."
    current_action = final_recommendation or "Monitor again later."

    urgency = "normal"

    # stage driven urgency first
    if current_stage_key == "spoiled":
        urgency = "discard"
    elif current_stage_key in {"overripe", "ripe_to_overripe"}:
        urgency = "high"
    elif current_stage_key in {"try_again", "weak_reading", "overexposed", "no_data", "unknown"}:
        urgency = "watch"

    # storage can raise urgency
    if current_storage_key == "poor":
        urgency = "high" if urgency != "discard" else urgency
    elif current_storage_key in {"warm", "dry", "humid", "cold"} and urgency == "normal":
        urgency = "watch"

    threshold_rows = [
        {
            "id": "good",
            "storage_state": "Good",
            "temp_rule": "13–20 °C",
            "rh_rule": "85–95 %",
            "meaning": "Storage looks acceptable right now.",
            "impact": "Normal holding condition.",
            "action": "Keep monitoring with regular scans.",
        },
        {
            "id": "warm",
            "storage_state": "Warm",
            "temp_rule": "> 20 °C",
            "rh_rule": "85–95 %",
            "meaning": "Too warm.",
            "impact": "Banana may ripen faster.",
            "action": "Move to a cooler place if possible.",
        },
        {
            "id": "cold",
            "storage_state": "Cold",
            "temp_rule": "< 13 °C",
            "rh_rule": "Any",
            "meaning": "Too cold.",
            "impact": "Peel can darken unnaturally.",
            "action": "Avoid colder holding conditions.",
        },
        {
            "id": "dry",
            "storage_state": "Dry",
            "temp_rule": "13–20 °C",
            "rh_rule": "< 85 %",
            "meaning": "Air is too dry.",
            "impact": "Fruit may dehydrate faster.",
            "action": "Increase humidity if possible.",
        },
        {
            "id": "humid",
            "storage_state": "Humid",
            "temp_rule": "13–20 °C",
            "rh_rule": "> 95 %",
            "meaning": "Air is too humid.",
            "impact": "Watch for moisture-related spoilage.",
            "action": "Reduce excess humidity if possible.",
        },
        {
            "id": "poor",
            "storage_state": "Poor",
            "temp_rule": "> 20 °C",
            "rh_rule": "< 85 %",
            "meaning": "Temperature and humidity are both unfavorable.",
            "impact": "Faster decline risk.",
            "action": "Correct both temperature and humidity quickly.",
        },
    ]

    stage_rows = [
        {"id": "green", "stage": "Green/Unripe", "meaning": "Not ready yet."},
        {"id": "unripe_to_ripe", "stage": "Proceeding from Unripe to Ripe", "meaning": "Ripening has started."},
        {"id": "ripe", "stage": "Ripe", "meaning": "Good to eat."},
        {"id": "ripe_to_overripe", "stage": "Proceeding from Ripe to Overripe", "meaning": "Still edible, but moving past the best eating window."},
        {"id": "overripe", "stage": "Overripe", "meaning": "Use today or very soon."},
        {"id": "spoiled", "stage": "Spoiled", "meaning": "Discard if obvious spoilage is present."},
    ]

    return {
        "temp_state": temp_state,
        "humidity_state": humidity_state,
        "current_summary": current_summary,
        "current_action": current_action,
        "urgency": urgency,
        "current_stage_key": current_stage_key,
        "current_storage_key": current_storage_key,
        "current_stage": stage,
        "current_storage": storage_label,
        "eat_recommendation": eat_recommendation,
        "final_recommendation": final_recommendation,
        "threshold_rows": threshold_rows,
        "stage_rows": stage_rows,
    }


def build_derived_payload(reading: Dict[str, Any]) -> Dict[str, Any]:
    readings = reading.get("readings") or {}
    env = reading.get("environment") or {}
    cls = reading.get("classification") or {}
    interp = reading.get("interpretation") or {}
    rec = reading.get("recommendations") or {}

    band_details = build_band_details(readings)
    strongest_any = strongest_band(band_details)
    strongest_visible = strongest_band(band_details, visible_only=True)
    top_visible = top_bands(band_details, visible_only=True)

    image_meta = read_json_file(LATEST_IMAGE_META_PATH) or {}
    image_exists = LATEST_IMAGE_PATH.exists()
    reading_sample_id = safe_int(reading.get("sample_id"))
    image_sample_id = safe_int(image_meta.get("sample_id"), -1)
    image_matched = image_exists and image_sample_id == reading_sample_id

    storage_guidance = build_storage_guidance(
        stage=str(cls.get("stage") or "Unknown"),
        storage_label=str(env.get("storage") or "Unknown"),
        storage_advice=str(rec.get("storage_advice") or "No storage advice available."),
        eat_recommendation=str(rec.get("eat_recommendation") or "Monitor again later."),
        final_recommendation=str(rec.get("final_recommendation") or "Need more scans for confidence."),
        temperature_c=safe_float(env.get("temperature_c"), None),
        humidity_rh=safe_float(env.get("humidity_rh"), None),
    )

    return {
        "band_details": band_details,
        "strongest_channel_detail": strongest_any,
        "strongest_visible_band_detail": strongest_visible,
        "top_visible_bands": top_visible,
        "spectral_story": build_story(reading, strongest_visible),
        "image": {
            "exists": image_exists,
            "matched": image_matched,
            "url": "/api/latest_image" if image_matched else None,
            "meta": image_meta if image_matched else None,
            "latest_meta": image_meta if image_exists else None,
        },
        "storage_guidance": storage_guidance,
        "summary": {
            "stage": str(cls.get("stage") or "Unknown"),
            "stage_detail": str(cls.get("stage_detail") or "No stage detail yet."),
            "stage_advice": str(cls.get("stage_advice") or "Check reading."),
            "reading_quality": str(interp.get("reading_quality") or "Unknown"),
            "reading_quality_advice": str(interp.get("reading_quality_advice") or "Check reading quality."),
            "storage": str(env.get("storage") or "Unknown"),
            "storage_advice": str(rec.get("storage_advice") or "No storage advice available."),
            "eat_recommendation": str(rec.get("eat_recommendation") or "Monitor again later."),
            "final_recommendation": str(rec.get("final_recommendation") or "Need more scans for confidence."),
            "spoilage_score": safe_int(cls.get("spoilage_score"), safe_int(reading.get("spoilage_score"), 0)),
        },
    }


def latest_payload() -> Dict[str, Any]:
    reading = read_json_file(LATEST_JSON_PATH) or {}
    if not reading:
        return {
            "ok": False,
            "server_time_utc": utc_now_iso(),
            "reading": {},
            "derived": {},
        }

    payload = {
        "ok": True,
        "server_time_utc": utc_now_iso(),
        "reading": reading,
        "derived": build_derived_payload(reading),
    }
    return make_json_safe(payload)



# FRONTEND TEMPLATE
# simple system so including every section in single file.

DASHBOARD_HTML = r"""
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Banana Monitor Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <style>
    :root {
      --bg: #f6f7fb;
      --bg-2: #eef2f8;
      --panel: rgba(255, 255, 255, 0.92);
      --panel-soft: #fafbff;
      --text: #1f2937;
      --muted: #667085;
      --line: #e7eaf3;
      --accent: #8b5cf6;
      --good: #16a34a;
      --warn: #d97706;
      --bad: #dc2626;
      --shadow: 0 14px 34px rgba(99, 102, 241, 0.08);
      --radius: 22px;
      --radius-sm: 16px;
    }

    * { box-sizing: border-box; min-width: 0; }
    html, body { margin: 0; padding: 0; }

    body {
      min-height: 100vh;
      background:
        radial-gradient(circle at top left, rgba(139, 92, 246, 0.08), transparent 28%),
        linear-gradient(180deg, var(--bg) 0%, var(--bg-2) 100%);
      color: var(--text);
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }

    .page {
      width: 100%;
      max-width: 1480px;
      margin: 0 auto;
      padding: 24px;
    }

    .header-shell {
      display: grid;
      gap: 14px;
      margin-bottom: 20px;
    }

    .hero-main {
      display: grid;
      grid-template-columns: auto 1fr;
      gap: 16px;
      align-items: start;
    }

    .hero-logo {
      width: 64px;
      height: 64px;
      border-radius: 20px;
      background: linear-gradient(135deg, #8b5cf6, #a78bfa);
      color: white;
      display: grid;
      place-items: center;
      font-size: 1.9rem;
      box-shadow: 0 14px 28px rgba(139, 92, 246, 0.22);
    }

    .hero-copy {
      display: grid;
      gap: 8px;
      text-align: left;
    }

    .hero-copy h1 {
      margin: 0;
      font-size: clamp(1.75rem, 3vw, 2.6rem);
      line-height: 1.08;
      letter-spacing: -0.02em;
      text-align: left;
    }

    .hero-copy p {
      margin: 0;
      color: var(--muted);
      font-size: 1rem;
      line-height: 1.42;
      text-align: left;
      max-width: 980px;
    }

    .toolbar {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      justify-content: flex-start;
      align-items: center;
    }

    .pill {
      display: inline-flex;
      align-items: center;
      justify-content: flex-start;
      min-height: 44px;
      padding: 10px 14px;
      background: rgba(255, 255, 255, 0.96);
      border: 1px solid var(--line);
      border-radius: 999px;
      box-shadow: var(--shadow);
      color: var(--muted);
      font-size: 0.94rem;
      text-align: left;
      white-space: normal;
      word-break: break-word;
    }

    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      min-height: 44px;
      padding: 10px 16px;
      border: 0;
      border-radius: 999px;
      background: linear-gradient(135deg, #8b5cf6, #7c3aed);
      color: white;
      font-weight: 700;
      cursor: pointer;
      box-shadow: 0 12px 24px rgba(124, 58, 237, 0.22);
    }

    .stack { display: grid; gap: 20px; }

    .card {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: var(--radius);
      box-shadow: var(--shadow);
      padding: 20px;
      text-align: left;
    }

    .card h2 {
      margin: 0 0 10px;
      font-size: 1.08rem;
      line-height: 1.2;
      text-align: left;
    }

    .subtle {
      color: var(--muted);
      font-size: 0.93rem;
      line-height: 1.5;
      text-align: left;
    }

    .value {
      font-size: 1.1rem;
      font-weight: 700;
      line-height: 1.35;
      text-align: left;
      word-break: break-word;
    }

    .top-grid {
      display: grid;
      grid-template-columns: minmax(0, 1.34fr) minmax(260px, 0.74fr);
      gap: 20px;
      align-items: start;
      margin-bottom: 20px;
    }

    .bottom-grid {
      display: grid;
      grid-template-columns: minmax(0, 1.35fr) minmax(320px, 0.95fr);
      gap: 20px;
      align-items: start;
    }

    .kpis {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 12px;
    }

    .kpi {
      background: var(--panel-soft);
      border: 1px solid var(--line);
      border-radius: var(--radius-sm);
      padding: 16px;
      text-align: left;
    }

    .kpi .label {
      color: var(--muted);
      font-size: 0.83rem;
      margin-bottom: 8px;
      text-align: left;
      text-transform: uppercase;
      letter-spacing: .03em;
    }

    .kpi .value {
      font-size: 1.08rem;
      font-weight: 700;
      line-height: 1.3;
      text-align: left;
    }

    .kpi-stage {
      color: #ffffff;
      border: 0;
      box-shadow: 0 14px 28px rgba(0, 0, 0, 0.08);
    }

    .kpi-stage .label,
    .kpi-stage .value {
      color: #ffffff;
    }

    .stage-green-bg { background: linear-gradient(135deg, #16a34a, #15803d); }
    .stage-unripe_to_ripe-bg { background: linear-gradient(135deg, #65a30d, #f59e0b); }
    .stage-ripe-bg { background: linear-gradient(135deg, #f59e0b, #ea580c); }
    .stage-ripe_to_overripe-bg { background: linear-gradient(135deg, #fb923c, #9a3412); }
    .stage-overripe-bg { background: linear-gradient(135deg, #c2410c, #9a3412); }
    .stage-spoiled-bg { background: linear-gradient(135deg, #dc2626, #7f1d1d); }
    .stage-try_again-bg { background: linear-gradient(135deg, #64748b, #475569); }
    .stage-weak_reading-bg { background: linear-gradient(135deg, #78716c, #57534e); }
    .stage-overexposed-bg { background: linear-gradient(135deg, #94a3b8, #475569); }
    .stage-no_data-bg { background: linear-gradient(135deg, #94a3b8, #64748b); }
    .stage-unknown-bg { background: linear-gradient(135deg, #94a3b8, #64748b); }

    .mini-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 12px;
    }

    .highlight {
      background: var(--panel-soft);
      border: 1px solid var(--line);
      border-radius: var(--radius-sm);
      padding: 16px;
      text-align: left;
    }

    .list { display: grid; gap: 10px; }

    .row {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      gap: 12px;
      padding: 12px 0;
      border-bottom: 1px solid var(--line);
      text-align: left;
    }

    .row:last-child { border-bottom: none; padding-bottom: 0; }
    .row:first-child { padding-top: 0; }

    .row-label {
      color: var(--muted);
      font-size: 0.93rem;
      line-height: 1.45;
      text-align: left;
    }

    .row-value {
      font-weight: 600;
      line-height: 1.45;
      text-align: left;
    }

    .status-good { color: var(--good); }
    .status-warn { color: var(--warn); }
    .status-bad  { color: var(--bad); }

    .image-box {
      min-height: 220px;
      max-height: 260px;
      background: linear-gradient(180deg, #fbfcff, #f5f7fc);
      border: 1px dashed #d7deee;
      border-radius: 18px;
      display: grid;
      place-items: center;
      overflow: hidden;
    }

    .image-box img {
      width: 100%;
      height: 100%;
      display: block;
      object-fit: cover;
    }

    .image-meta { margin-top: 12px; display: grid; gap: 8px; }

    .empty {
      color: var(--muted);
      text-align: center;
      padding: 24px;
      line-height: 1.6;
    }

    .chart-shell {
      background: linear-gradient(180deg, #ffffff, #fbfcff);
      border: 1px solid var(--line);
      border-radius: 18px;
      padding: 12px;
      overflow-x: auto;
    }

    svg text { fill: var(--muted); font-size: 12px; }
    .axis { stroke: #c8d0e0; stroke-width: 1; }
    .gridline { stroke: rgba(156, 163, 175, 0.18); stroke-width: 1; }

    details.panel-section,
    details.band-section {
      border: 1px solid var(--line);
      border-radius: 18px;
      background: var(--panel-soft);
      overflow: hidden;
    }

    details.panel-section summary,
    details.band-section summary {
      list-style: none;
      cursor: pointer;
      padding: 18px 20px;
      display: flex;
      align-items: flex-start;
      justify-content: space-between;
      gap: 12px;
      font-weight: 700;
      user-select: none;
      text-align: left;
    }

    details.panel-section summary::-webkit-details-marker,
    details.band-section summary::-webkit-details-marker { display: none; }

    .summary-left {
      display: grid;
      gap: 4px;
      text-align: left;
      flex: 1 1 auto;
      min-width: 0;
    }

    .caret {
      width: 34px;
      height: 34px;
      border-radius: 999px;
      display: grid;
      place-items: center;
      background: white;
      border: 1px solid var(--line);
      color: var(--accent);
      transition: transform 0.2s ease;
      flex: 0 0 auto;
    }

    details[open] .caret { transform: rotate(180deg); }

    .band-section-body,
    .panel-section-body {
      padding: 0 20px 20px;
      display: grid;
      gap: 12px;
    }

    .band-card {
      background: white;
      border: 1px solid var(--line);
      border-radius: 16px;
      padding: 14px;
      display: grid;
      gap: 10px;
      text-align: left;
    }

    .band-head {
      display: flex;
      justify-content: space-between;
      gap: 12px;
      align-items: flex-start;
      text-align: left;
    }

    .band-title {
      font-weight: 700;
      display: flex;
      align-items: center;
      gap: 10px;
      flex-wrap: wrap;
      text-align: left;
    }

    .dot {
      width: 12px;
      height: 12px;
      border-radius: 999px;
      flex: 0 0 auto;
    }

    .chip {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      background: #ffffff;
      border: 1px solid var(--line);
      border-radius: 999px;
      padding: 6px 10px;
      font-size: 0.84rem;
      color: var(--muted);
      white-space: nowrap;
      text-align: left;
    }

    .meter {
      height: 10px;
      background: #eef2f8;
      border: 1px solid #e1e7f0;
      border-radius: 999px;
      overflow: hidden;
    }

    .meter > span {
      display: block;
      height: 100%;
      width: 0%;
      border-radius: 999px;
    }

    .table-wrap {
      overflow-x: auto;
      border: 1px solid var(--line);
      border-radius: 18px;
      background: white;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      min-width: 720px;
    }

    thead th {
      text-align: left;
      font-size: 0.94rem;
      padding: 14px 16px;
      border-bottom: 2px solid var(--text);
      background: #fbfcff;
    }

    tbody td {
      padding: 14px 16px;
      border-bottom: 1px solid var(--line);
      vertical-align: top;
      text-align: left;
      line-height: 1.5;
    }

    tbody tr:last-child td { border-bottom: none; }
    tbody tr.active-row { background: rgba(139, 92, 246, 0.06); }

    .urgency-good { color: var(--good); }
    .urgency-watch { color: var(--warn); }
    .urgency-high, .urgency-discard { color: var(--bad); }

    pre {
      margin: 0;
      white-space: pre-wrap;
      word-wrap: break-word;
      background: #ffffff;
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 14px;
      font-size: 12px;
      color: #334155;
      overflow: auto;
      text-align: left;
      max-height: 420px;
    }

    .placeholder-box {
      background: linear-gradient(180deg, #ffffff, #fafcff);
      border: 1px dashed var(--line);
      border-radius: 18px;
      padding: 18px;
      text-align: left;
      color: var(--muted);
    }

    @media (max-width: 1120px) {
      .top-grid, .bottom-grid { grid-template-columns: 1fr; }
    }

    @media (max-width: 820px) {
      .page { padding: 18px; }
      .kpis { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .mini-grid { grid-template-columns: 1fr; }
    }

    @media (max-width: 640px) {
      .page { padding: 16px; }
      .header-shell { gap: 12px; margin-bottom: 16px; }
      .hero-main { grid-template-columns: 1fr; gap: 12px; }
      .hero-logo { width: 60px; height: 60px; border-radius: 18px; }
      .toolbar { display: grid; grid-template-columns: 1fr; gap: 10px; align-items: stretch; justify-items: stretch; }
      .pill, .btn { width: 100%; justify-content: flex-start; text-align: left; }
      .kpis { grid-template-columns: 1fr; }
      .row, .band-head { flex-direction: column; justify-content: flex-start; align-items: flex-start; gap: 6px; }
      .image-box { min-height: 180px; max-height: 220px; }
      .empty { text-align: left; }
    }
  </style>
</head>
<body>
  <div class="page">
    <header class="header-shell">
      <div class="hero-main">
        <div class="hero-logo">🍌</div>
        <div class="hero-copy">
          <h1>Banana Monitor Dashboard</h1>
          <p>This page shows the latest ESP32 reading, image, ratios, spectral channels, and storage interpretation using the same field structure as the final sketch.</p>
        </div>
      </div>

      <div class="toolbar">
        <button class="btn" onclick="loadDashboard()">Refresh now</button>
        <div class="pill" id="pill-device">Device: waiting...</div>
        <div class="pill" id="pill-sample">Sample ID: --</div>
        <div class="pill" id="pill-next">Next data in: --:--</div>
      </div>
    </header>

    <main>
      <section class="top-grid">
        <div class="card stack">
          <div>
            <h2>Overview</h2>
            <div class="subtle">Main reading summary from the current ESP32 sample.</div>
          </div>

          <div class="kpis" id="overview-kpis">
            <div class="kpi"><div class="label">Banana stage</div><div class="value">Waiting for reading</div></div>
            <div class="kpi"><div class="label">Reading quality</div><div class="value">—</div></div>
            <div class="kpi"><div class="label">Storage state</div><div class="value">—</div></div>
            <div class="kpi"><div class="label">Eat recommendation</div><div class="value">—</div></div>
          </div>

          <div class="mini-grid">
            <div class="highlight">
              <div class="subtle">Spectral summary</div>
              <div id="spectral-story" class="value">No spectral data received yet.</div>
            </div>
            <div class="highlight">
              <div class="subtle">Strongest visible band</div>
              <div id="strongest-band" class="value">Waiting for data</div>
            </div>
          </div>

          <div class="list" id="overview-list">
            <div class="placeholder-box">Waiting for first reading.</div>
          </div>
        </div>

        <div class="card">
          <h2>Latest matched image</h2>
          <div class="subtle">The image is shown only when its sample_id matches the current reading.</div>
          <div style="height:12px"></div>

          <div class="image-box" id="image-box">
            <div class="empty">No matched image yet.<br>The dashboard will show it automatically once the current reading and image belong to the same sample.</div>
          </div>

          <div class="image-meta" id="image-meta"></div>
        </div>
      </section>

      <section class="bottom-grid">
        <div class="stack">
          <section class="card">
            <h2>All spectral channels</h2>
            <div class="subtle">Live spectrum chart for F1–F8, Clear, and NIR.</div>
            <div style="height:12px"></div>
            <div class="chart-shell" id="spectrum-chart-shell">
              <div class="placeholder-box">No chart yet. The dashboard will draw the spectrum after the first reading arrives.</div>
            </div>
          </section>

          <section class="card">
            <details class="band-section" id="band-details-wrap">
              <summary>
                <div class="summary-left">
                  <div>Band details and interpretation</div>
                  <div class="subtle">Collapsed by default. Open when you want the full wavelength-by-wavelength breakdown.</div>
                </div>
                <div class="caret">⌄</div>
              </summary>

              <div class="band-section-body">
                <div class="list" id="band-list">
                  <div class="placeholder-box">No band details yet. They will appear here after the first reading.</div>
                </div>
              </div>
            </details>
          </section>

          <section class="card">
            <details class="panel-section">
              <summary>
                <div class="summary-left">
                  <div>Raw JSON payload</div>
                  <div class="subtle">Hidden by default so the page stays cleaner.</div>
                </div>
                <div class="caret">⌄</div>
              </summary>
              <div class="panel-section-body">
                <pre id="json-dump">No data yet</pre>
              </div>
            </details>
          </section>
        </div>

        <div class="stack">
          <section class="card">
            <h2>Environment and device status</h2>
            <div class="list" id="env-list">
              <div class="placeholder-box">Waiting for environment and device data.</div>
            </div>
          </section>

          <section class="card">
            <h2>Ratio summary</h2>
            <div class="subtle">Quick view of the ratios sent by the final sketch.</div>
            <div style="height:12px"></div>
            <div id="ratio-list">
              <div class="placeholder-box">No ratio data yet.</div>
            </div>
          </section>

          <section class="card">
            <h2>Storage interpretation</h2>
            <div class="subtle">Uses the same storage labels and thresholds as the final sketch.</div>
            <div style="height:12px"></div>

            <div class="mini-grid">
              <div class="highlight">
                <div class="subtle">Current storage summary</div>
                <div id="storage-summary" class="value">No guidance yet.</div>
              </div>
              <div class="highlight">
                <div class="subtle">Current action</div>
                <div id="storage-action" class="value">Waiting for data.</div>
              </div>
            </div>

            <div style="height:12px"></div>

            <div class="list" id="storage-flags">
              <div class="placeholder-box">No storage guidance yet.</div>
            </div>

            <div style="height:12px"></div>

            <div class="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Storage state</th>
                    <th>Temp rule</th>
                    <th>RH rule</th>
                    <th>Meaning</th>
                    <th>Impact</th>
                    <th>Suggested action</th>
                  </tr>
                </thead>
                <tbody id="guidance-table-body">
                  <tr>
                    <td colspan="6" class="subtle">No guidance table yet.</td>
                  </tr>
                </tbody>
              </table>
            </div>
          </section>
        </div>
      </section>
    </main>
  </div>

  <script>
    let nextExpectedMs = null;

    function escapeHtml(text) {
      return String(text ?? '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#039;');
    }

    function num(value, digits = 4) {
      const n = Number(value);
      if (!Number.isFinite(n)) return '--';
      return n.toFixed(digits);
    }

    function temp(value) {
      const n = Number(value);
      if (!Number.isFinite(n)) return '--';
      return n.toFixed(2) + ' °C';
    }

    function rh(value) {
      const n = Number(value);
      if (!Number.isFinite(n)) return '--';
      return n.toFixed(2) + ' %';
    }

    function statusClass(value) {
      const v = String(value || '').toLowerCase();
      if (v.includes('good') || v.includes('usable') || v === 'ripe' || v.includes('acceptable')) return 'status-good';
      if (v.includes('warm') || v.includes('humid') || v.includes('dry') || v.includes('cold') || v.includes('overripe') || v.includes('mixed') || v.includes('try again')) return 'status-warn';
      if (v.includes('spoiled') || v.includes('bad') || v.includes('dark') || v.includes('fail') || v.includes('discard') || v.includes('poor')) return 'status-bad';
      return '';
    }

    function urgencyClass(value) {
      const v = String(value || '').toLowerCase();
      if (v === 'normal') return 'urgency-good';
      if (v === 'watch') return 'urgency-watch';
      if (v === 'high') return 'urgency-high';
      if (v === 'discard') return 'urgency-discard';
      return '';
    }

    function rowHtml(label, value) {
      return `
        <div class="row">
          <div class="row-label">${label}</div>
          <div class="row-value">${value}</div>
        </div>
      `;
    }

    function stageTheme(stage) {
      const s = String(stage || '').toLowerCase();

      if (s.includes('proceeding from unripe to ripe')) return 'stage-unripe_to_ripe-bg';
      if (s.includes('proceeding from ripe to overripe')) return 'stage-ripe_to_overripe-bg';
      if (s.includes('spoiled')) return 'stage-spoiled-bg';
      if (s.includes('overripe')) return 'stage-overripe-bg';
      if (s === 'ripe') return 'stage-ripe-bg';
      if (s.includes('green') || s.includes('unripe')) return 'stage-green-bg';
      if (s.includes('try again')) return 'stage-try_again-bg';
      if (s.includes('weak reading')) return 'stage-weak_reading-bg';
      if (s.includes('overexposed reading')) return 'stage-overexposed-bg';
      if (s.includes('no data')) return 'stage-no_data-bg';
      return 'stage-unknown-bg';
    }

    function formatCountdown(msRemaining) {
      if (!Number.isFinite(msRemaining)) return '--:--';
      if (msRemaining <= 0) return 'due now';
      const totalSec = Math.floor(msRemaining / 1000);
      const hours = Math.floor(totalSec / 3600);
      const minutes = Math.floor((totalSec % 3600) / 60);
      const seconds = totalSec % 60;
      if (hours > 0) {
        return String(hours).padStart(2, '0') + ':' + String(minutes).padStart(2, '0') + ':' + String(seconds).padStart(2, '0');
      }
      return String(minutes).padStart(2, '0') + ':' + String(seconds).padStart(2, '0');
    }

    function updateNextCountdown() {
      const pill = document.getElementById('pill-next');
      if (!pill) return;
      if (!Number.isFinite(nextExpectedMs)) {
        pill.textContent = 'Next data in: --:--';
        return;
      }
      pill.textContent = 'Next data in: ' + formatCountdown(nextExpectedMs - Date.now());
    }

    function buildSpectrumSVG(bands) {
      const width = 980;
      const height = 340;
      const margin = { top: 22, right: 26, bottom: 90, left: 56 };
      const chartW = width - margin.left - margin.right;
      const chartH = height - margin.top - margin.bottom;
      const values = bands.map(b => Number(b.value || 0));
      const maxVal = Math.max(...values, 1);
      const step = chartW / bands.length;
      const barW = Math.max(26, step * 0.58);
      const topKey = [...bands].sort((a, b) => Number(b.value || 0) - Number(a.value || 0))[0]?.key;

      let svg = `<svg viewBox="0 0 ${width} ${height}" width="100%" height="100%" role="img" aria-label="Spectrum chart">`;
      svg += `<line class="axis" x1="${margin.left}" y1="${margin.top + chartH}" x2="${margin.left + chartW}" y2="${margin.top + chartH}" />`;
      svg += `<line class="axis" x1="${margin.left}" y1="${margin.top}" x2="${margin.left}" y2="${margin.top + chartH}" />`;

      for (let i = 0; i <= 4; i++) {
        const y = margin.top + chartH - (chartH * i / 4);
        const label = Math.round(maxVal * i / 4);
        svg += `<line class="gridline" x1="${margin.left}" y1="${y}" x2="${margin.left + chartW}" y2="${y}" />`;
        svg += `<text x="${margin.left - 8}" y="${y + 4}" text-anchor="end">${label}</text>`;
      }

      bands.forEach((band, idx) => {
        const x = margin.left + idx * step + (step - barW) / 2;
        const value = Number(band.value || 0);
        const h = (value / maxVal) * chartH;
        const y = margin.top + chartH - h;
        const opacity = band.key === topKey ? 1 : 0.9;
        const nmLabel = band.nm ? `${band.nm} nm` : band.label;

        svg += `<rect x="${x}" y="${y}" width="${barW}" height="${Math.max(h, 2)}" rx="7" fill="${band.color}" opacity="${opacity}" />`;
        svg += `<text x="${x + barW / 2}" y="${margin.top + chartH + 20}" text-anchor="middle">${band.title}</text>`;
        svg += `<text x="${x + barW / 2}" y="${margin.top + chartH + 38}" text-anchor="middle">${nmLabel}</text>`;
        svg += `<text x="${x + barW / 2}" y="${Math.max(y - 8, 14)}" text-anchor="middle">${value}</text>`;
      });

      svg += `</svg>`;
      return svg;
    }

    function renderOverview(payload) {
      const derived = payload.derived || {};
      const summary = derived.summary || {};
      const storageGuidance = derived.storage_guidance || {};
      const strongest = derived.strongest_visible_band_detail || {};
      const stageThemeClass = stageTheme(summary.stage || '');

      const kpis = [
        { label: 'Banana stage', value: escapeHtml(summary.stage || '--'), extraClass: `kpi-stage ${stageThemeClass}` },
        { label: 'Reading quality', value: `<span class="${statusClass(summary.reading_quality)}">${escapeHtml(summary.reading_quality || '--')}</span>`, extraClass: '' },
        { label: 'Storage state', value: `<span class="${statusClass(summary.storage)}">${escapeHtml(summary.storage || '--')}</span>`, extraClass: '' },
        { label: 'Eat recommendation', value: escapeHtml(summary.eat_recommendation || '--'), extraClass: '' },
      ];

      document.getElementById('overview-kpis').innerHTML = kpis.map(k => `
        <div class="kpi ${k.extraClass}">
          <div class="label">${k.label}</div>
          <div class="value">${k.value}</div>
        </div>
      `).join('');

      document.getElementById('spectral-story').innerHTML = escapeHtml(derived.spectral_story || '--');

      const strongestText = strongest.key ? `
        <span style="display:inline-flex;align-items:center;gap:10px;flex-wrap:wrap;">
          <span class="dot" style="background:${escapeHtml(strongest.color)}"></span>
          <span>${escapeHtml(strongest.title)}${strongest.nm ? ` (${strongest.nm} nm)` : ''} • ${escapeHtml(strongest.label)} • ${escapeHtml(strongest.value)}</span>
        </span>
      ` : 'Waiting for data';
      document.getElementById('strongest-band').innerHTML = strongestText;

      document.getElementById('overview-list').innerHTML = [
        rowHtml('Stage detail', escapeHtml(summary.stage_detail || '--')),
        rowHtml('Stage advice', escapeHtml(summary.stage_advice || '--')),
        rowHtml('Reading quality advice', escapeHtml(summary.reading_quality_advice || '--')),
        rowHtml('Storage advice', escapeHtml(summary.storage_advice || '--')),
        rowHtml('Final recommendation', escapeHtml(summary.final_recommendation || '--')),
        rowHtml('Spoilage score', escapeHtml(summary.spoilage_score ?? '--')),
        rowHtml('Dynamic urgency', `<span class="${urgencyClass(storageGuidance.urgency)}">${escapeHtml(storageGuidance.urgency || '--')}</span>`),
      ].join('');
    }

    function renderEnvironment(payload) {
      const reading = payload.reading || {};
      const env = reading.environment || {};
      const camera = reading.camera || {};
      const image = (payload.derived || {}).image || {};

      const rows = [
        { label: 'Temperature', value: temp(env.temperature_c) },
        { label: 'Humidity', value: rh(env.humidity_rh) },
        { label: 'Wi-Fi RSSI', value: reading.wifi_rssi !== undefined ? `${reading.wifi_rssi} dBm` : '--' },
        { label: 'Sensor LED during read', value: reading.led_enabled ? `ON (${reading.led_current_ma || 0} mA)` : 'OFF' },
        { label: 'Camera capture', value: camera.capture_ok === true ? 'OK' : camera.capture_ok === false ? 'FAIL' : '--' },
        { label: 'Image matched to sample', value: image.matched ? 'YES' : 'NO' },
        { label: 'Image size', value: camera.width && camera.height ? `${camera.width} × ${camera.height}` : '--' },
        { label: 'JPEG bytes', value: camera.jpeg_bytes ? String(camera.jpeg_bytes) : '--' },
        { label: 'Sample interval', value: reading.sample_interval_sec ? `${reading.sample_interval_sec} sec` : '--' },
        { label: 'Device uptime at send', value: reading.device_millis !== undefined ? `${reading.device_millis} ms` : '--' },
      ];

      document.getElementById('env-list').innerHTML = rows.map(row =>
        rowHtml(escapeHtml(row.label), escapeHtml(row.value))
      ).join('');
    }

    function renderRatios(payload) {
      const ratios = (payload.reading || {}).ratios || {};
      const labels = {
        nir_to_clear: 'NIR / Clear',
        f5_to_f7: 'F5 / F7',
        f7_to_f5: 'F7 / F5',
        f4_to_f8: 'F4 / F8',
        f8_to_f6: 'F8 / F6',
      };

      document.getElementById('ratio-list').innerHTML = Object.entries(labels).map(([key, label]) =>
        rowHtml(escapeHtml(label), escapeHtml(num(ratios[key], 4)))
      ).join('');
    }

    function renderBands(payload) {
      const bands = (payload.derived || {}).band_details || [];
      if (!bands.length) {
        document.getElementById('band-list').innerHTML = '<div class="placeholder-box">No band details yet. They will appear here after the first reading.</div>';
        return;
      }

      document.getElementById('band-list').innerHTML = bands.map(band => `
        <div class="band-card">
          <div class="band-head">
            <div>
              <div class="band-title">
                <span class="dot" style="background:${escapeHtml(band.color)}"></span>
                <span>${escapeHtml(band.title)} • ${band.nm ? escapeHtml(band.nm + ' nm') : escapeHtml(band.label)}</span>
              </div>
              <div class="subtle">${escapeHtml(band.meaning)}</div>
            </div>
            <div class="chip">${escapeHtml(band.level)}</div>
          </div>

          <div class="row" style="padding:0;border-bottom:none;">
            <div class="row-label">Raw value</div>
            <div class="row-value"><strong>${escapeHtml(band.value)}</strong></div>
          </div>

          <div class="row" style="padding:0;border-bottom:none;">
            <div class="row-label">Relative to strongest channel</div>
            <div class="row-value"><strong>${escapeHtml(band.relative_percent_of_peak)}%</strong></div>
          </div>

          <div class="meter"><span style="width:${escapeHtml(band.relative_percent_of_peak)}%;background:${escapeHtml(band.color)};"></span></div>
        </div>
      `).join('');
    }

    function renderChart(payload) {
      const bands = (payload.derived || {}).band_details || [];
      if (!bands.length) {
        document.getElementById('spectrum-chart-shell').innerHTML = '<div class="placeholder-box">No chart yet. The dashboard will draw the spectrum after the first reading arrives.</div>';
        return;
      }
      document.getElementById('spectrum-chart-shell').innerHTML = buildSpectrumSVG(bands);
    }

    function renderImage(payload) {
      const image = (payload.derived || {}).image || {};
      const box = document.getElementById('image-box');
      const metaWrap = document.getElementById('image-meta');
      const imageMeta = image.meta || {};

      if (image.matched && image.url) {
        const url = image.url + '?t=' + Date.now();
        box.innerHTML = `<img src="${url}" alt="Matched uploaded fruit image" />`;

        const metaRows = [
          { label: 'Matched sample ID', value: String(imageMeta.sample_id ?? '--') },
          { label: 'Uploaded at', value: String(imageMeta.uploaded_at_utc ?? '--') },
          { label: 'Dimensions', value: imageMeta.width && imageMeta.height ? `${imageMeta.width} × ${imageMeta.height}` : '--' },
          { label: 'JPEG bytes', value: imageMeta.jpeg_bytes ? String(imageMeta.jpeg_bytes) : '--' },
        ];

        metaWrap.innerHTML = metaRows.map(row =>
          rowHtml(escapeHtml(row.label), escapeHtml(row.value))
        ).join('');
      } else {
        box.innerHTML = `<div class="empty">No matched image yet.<br>The dashboard shows the image only when its sample_id matches the current reading.</div>`;
        const latestMeta = image.latest_meta || {};
        if (Object.keys(latestMeta).length) {
          metaWrap.innerHTML = [
            rowHtml('Latest saved image sample', escapeHtml(String(latestMeta.sample_id ?? '--'))),
            rowHtml('Current reading sample', escapeHtml(String((payload.reading || {}).sample_id ?? '--'))),
          ].join('');
        } else {
          metaWrap.innerHTML = '';
        }
      }
    }

    function renderStorageGuidance(payload) {
      const guidance = ((payload.derived || {}).storage_guidance) || {};
      const reading = payload.reading || {};
      const env = reading.environment || {};

      document.getElementById('storage-summary').textContent = guidance.current_summary || 'No guidance yet.';
      document.getElementById('storage-action').textContent = guidance.current_action || 'Waiting for data.';

      document.getElementById('storage-flags').innerHTML = [
        rowHtml('Current stage', escapeHtml(guidance.current_stage || '--')),
        rowHtml('Current storage label', `<span class="${statusClass(guidance.current_storage)}">${escapeHtml(guidance.current_storage || '--')}</span>`),
        rowHtml('Measured temperature', escapeHtml(temp(env.temperature_c))),
        rowHtml('Measured humidity', escapeHtml(rh(env.humidity_rh))),
        rowHtml('Temperature state', `<span class="${statusClass(guidance.temp_state)}">${escapeHtml(guidance.temp_state || '--')}</span>`),
        rowHtml('Humidity state', `<span class="${statusClass(guidance.humidity_state)}">${escapeHtml(guidance.humidity_state || '--')}</span>`),
        rowHtml('Eat recommendation', escapeHtml(guidance.eat_recommendation || '--')),
        rowHtml('Final recommendation', escapeHtml(guidance.final_recommendation || '--')),
        rowHtml('Urgency', `<span class="${urgencyClass(guidance.urgency)}">${escapeHtml(guidance.urgency || '--')}</span>`),
      ].join('');

      const rows = guidance.threshold_rows || [];
      const currentKey = guidance.current_storage_key;
      if (!rows.length) {
        document.getElementById('guidance-table-body').innerHTML = '<tr><td colspan="6" class="subtle">No guidance table yet.</td></tr>';
        return;
      }

      document.getElementById('guidance-table-body').innerHTML = rows.map(row => `
        <tr class="${row.id === currentKey ? 'active-row' : ''}">
          <td>${escapeHtml(row.storage_state)}</td>
          <td>${escapeHtml(row.temp_rule)}</td>
          <td>${escapeHtml(row.rh_rule)}</td>
          <td>${escapeHtml(row.meaning)}</td>
          <td>${escapeHtml(row.impact)}</td>
          <td>${escapeHtml(row.action)}</td>
        </tr>
      `).join('');
    }

    function updatePills(payload) {
      const reading = payload.reading || {};
      document.getElementById('pill-device').textContent = `Device: ${reading.device_id || 'waiting...'}`;
      document.getElementById('pill-sample').textContent = `Sample ID: ${reading.sample_id ?? '--'}`;

      const intervalSec = Number(reading.sample_interval_sec || 0);
      const receivedAtIso = reading.server_received_at_utc || '';
      const receivedAtMs = Date.parse(receivedAtIso);
      if (Number.isFinite(intervalSec) && intervalSec > 0 && Number.isFinite(receivedAtMs)) {
        nextExpectedMs = receivedAtMs + intervalSec * 1000;
      } else {
        nextExpectedMs = null;
      }
      updateNextCountdown();
    }

    async function loadDashboard() {
      try {
        const response = await fetch('/api/latest?_=' + Date.now(), { cache: 'no-store' });
        const payload = await response.json();

        if (!payload.ok) {
          document.getElementById('json-dump').textContent = 'No reading has been received yet.';
          nextExpectedMs = null;
          updateNextCountdown();
          return;
        }

        updatePills(payload);
        renderOverview(payload);
        renderEnvironment(payload);
        renderRatios(payload);
        renderBands(payload);
        renderChart(payload);
        renderImage(payload);
        renderStorageGuidance(payload);
        document.getElementById('json-dump').textContent = JSON.stringify(payload.reading || {}, null, 2);
      } catch (error) {
        document.getElementById('json-dump').textContent = 'Failed to load dashboard data: ' + error;
        nextExpectedMs = null;
        updateNextCountdown();
      }
    }

    loadDashboard();
    setInterval(loadDashboard, 5000);
    setInterval(updateNextCountdown, 1000);
  </script>
</body>
</html>
"""



# ROUTES
@app.get("/")
def dashboard() -> str:
    return render_template_string(DASHBOARD_HTML)


@app.get("/api/health")
def health() -> Any:
    return jsonify({"ok": True, "server_time_utc": utc_now_iso()})


@app.get("/api/latest")
def api_latest() -> Any:
    return jsonify(make_json_safe(latest_payload()))


@app.get("/api/latest_image")
def api_latest_image() -> Any:
    if not LATEST_IMAGE_PATH.exists():
        return jsonify({"ok": False, "error": "No image uploaded yet"}), 404
    return send_file(LATEST_IMAGE_PATH, mimetype="image/jpeg", max_age=0)


@app.post("/api/upload_image")
def upload_image() -> Any:
    image_bytes: bytes | None = None
    mime_type = request.content_type or "application/octet-stream"

    if "image" in request.files:
        image_file = request.files["image"]
        image_bytes = image_file.read()
        if image_file.mimetype:
            mime_type = image_file.mimetype
    else:
        image_bytes = request.get_data(cache=False, as_text=False)

    if not image_bytes:
        return jsonify({"ok": False, "error": "Empty image payload"}), 400

    sample_id = request.headers.get("X-Sample-Id") or request.form.get("sample_id")
    device_id = request.headers.get("X-Device-Id") or request.form.get("device_id")
    width = request.headers.get("X-Image-Width") or request.form.get("width")
    height = request.headers.get("X-Image-Height") or request.form.get("height")
    jpeg_bytes = request.headers.get("X-Jpeg-Bytes") or request.form.get("jpeg_bytes")

    image_meta = {
        "uploaded_at_utc": utc_now_iso(),
        "sample_id": safe_int(sample_id),
        "device_id": device_id,
        "content_type": mime_type,
        "width": safe_int(width),
        "height": safe_int(height),
        "jpeg_bytes": safe_int(jpeg_bytes, len(image_bytes)) if jpeg_bytes is not None else len(image_bytes),
        "server_received_bytes": len(image_bytes),
    }

    image_meta = make_json_safe(image_meta)

    with STATE_LOCK:
        atomic_write_bytes(LATEST_IMAGE_PATH, image_bytes)
        atomic_write_text(
            LATEST_IMAGE_META_PATH,
            json.dumps(image_meta, ensure_ascii=False, indent=2, allow_nan=False),
        )

    return jsonify({"ok": True, "message": "Image stored", "image": image_meta}), 200


@app.post("/api/reading")
def upload_reading() -> Any:
    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"ok": False, "error": "Invalid JSON body"}), 400

    payload = json.loads(json.dumps(payload))
    payload = make_json_safe(payload)
    payload["server_received_at_utc"] = utc_now_iso()

    with STATE_LOCK:
        atomic_write_text(
            LATEST_JSON_PATH,
            json.dumps(payload, ensure_ascii=False, indent=2, allow_nan=False),
        )
        append_jsonl(LOG_JSONL_PATH, payload)

    classification = payload.get("classification") or {}
    camera = payload.get("camera") or {}

    summary = {
        "sample_id": payload.get("sample_id"),
        "device_id": payload.get("device_id"),
        "stage": classification.get("stage"),
        "image_uploaded_flag": camera.get("image_uploaded"),
        "server_received_at_utc": payload.get("server_received_at_utc"),
    }
    summary = make_json_safe(summary)

    return jsonify({"ok": True, "message": "Reading stored", "summary": summary}), 200


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5002, debug=True)
    
#port 5002 chosen to avoid conflict with common Flask default port 5000, which might be used by other services or development servers in mac.