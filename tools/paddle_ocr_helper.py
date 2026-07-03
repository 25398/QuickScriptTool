#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PaddleOCR helper — called by QuickScriptTool C++ OCR engine."""

import argparse
import json
import os
import sys

# Avoid Unicode user-profile paths (Paddle Inference cannot open models there on Windows).
MODEL_ROOT = r"C:\paddle_env\models\whl"
DIGIT_CHARSET = set("0123456789.,/-%+:$")

_digit_rec_cache: dict[str, object] = {}


def model_dirs(lang: str) -> dict[str, str]:
    if lang == "ch":
        return {
            "det_model_dir": os.path.join(MODEL_ROOT, "det", "ch", "ch_PP-OCRv4_det_infer"),
            "rec_model_dir": os.path.join(MODEL_ROOT, "rec", "ch", "ch_PP-OCRv4_rec_infer"),
            "cls_model_dir": os.path.join(MODEL_ROOT, "cls", "ch_ppocr_mobile_v2.0_cls_infer"),
        }
    return {}


def parse_result(result) -> list[dict]:
    lines = []
    if not result:
        return lines

    def add_line(text, conf, box=None):
        if not text:
            return
        if box and isinstance(box, (list, tuple)) and len(box) >= 4:
            xs = [float(p[0]) for p in box]
            ys = [float(p[1]) for p in box]
            x1, y1, x2, y2 = int(min(xs)), int(min(ys)), int(max(xs)), int(max(ys))
        else:
            x1 = y1 = x2 = y2 = 0
        lines.append({
            "text": text,
            "x1": x1,
            "y1": y1,
            "x2": x2,
            "y2": y2,
            "confidence": float(conf),
        })

    for page in result:
        if not page:
            continue
        for block in page:
            if block is None:
                continue
            if isinstance(block, (list, tuple)) and len(block) == 2 and isinstance(block[1], (list, tuple)):
                box = block[0]
                text_info = block[1]
                if text_info and len(text_info) >= 1:
                    add_line(text_info[0], text_info[1] if len(text_info) > 1 else 0.0, box)
            elif isinstance(block, (list, tuple)) and len(block) >= 2 and isinstance(block[0], str):
                add_line(block[0], block[1] if len(block) > 1 else 0.0)
            elif isinstance(block, (list, tuple)) and len(block) >= 1:
                inner = block[0]
                if isinstance(inner, (list, tuple)) and len(inner) >= 2 and isinstance(inner[0], str):
                    add_line(inner[0], inner[1] if len(inner) > 1 else 0.0)
    return lines


def filter_digit_text(text: str) -> str:
    return "".join(ch for ch in text if ch in DIGIT_CHARSET)


def filter_digit_lines(lines: list[dict]) -> list[dict]:
    filtered = []
    for line in lines:
        text = filter_digit_text(line.get("text", ""))
        if text:
            entry = dict(line)
            entry["text"] = text
            filtered.append(entry)
    return filtered


def read_bgr_image(image_path: str):
    import cv2

    return cv2.imread(image_path)


def emit_json(payload: dict) -> None:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


def create_ocr(lang: str):
    os.environ["PADDLEOCR_HOME"] = r"C:\paddle_env\models"
    from paddleocr import PaddleOCR

    cpu = os.cpu_count() or 4
    base = {
        "use_angle_cls": True,
        "lang": lang,
        "show_log": False,
        "use_mkldnn": True,
        "cpu_threads": max(1, min(4, cpu)),
        **model_dirs(lang),
    }
    try:
        return PaddleOCR(**base)
    except Exception:
        base.pop("use_mkldnn", None)
        return PaddleOCR(**base)


def create_digit_recognizer(lang: str):
    """Rec-only engine: skips det/cls model init entirely."""
    os.environ["PADDLEOCR_HOME"] = r"C:\paddle_env\models"
    from pathlib import Path

    import paddleocr
    from paddleocr.paddleocr import get_model_config, parse_args
    from paddleocr.tools.infer.predict_rec import TextRecognizer

    params = parse_args(mMain=False)
    params.lang = lang
    params.show_log = False
    params.use_gpu = False
    params.enable_mkldnn = True
    params.cpu_threads = max(1, min(4, os.cpu_count() or 4))
    params.ocr_version = "PP-OCRv4"
    params.rec_model_dir = os.path.join(MODEL_ROOT, "rec", "ch", "ch_PP-OCRv4_rec_infer")
    params.rec_image_shape = "3, 48, 320"
    rec_cfg = get_model_config("OCR", params.ocr_version, "rec", lang)
    params.rec_char_dict_path = str(
        Path(paddleocr.__file__).parent / rec_cfg["dict_path"])

    try:
        return TextRecognizer(params)
    except Exception:
        params.enable_mkldnn = False
        return TextRecognizer(params)


def get_digit_recognizer(lang: str):
    if lang not in _digit_rec_cache:
        _digit_rec_cache[lang] = create_digit_recognizer(lang)
    return _digit_rec_cache[lang]


def run_digits_only(lang: str, image_path: str) -> dict:
    recognizer = get_digit_recognizer(lang)
    img = read_bgr_image(image_path)
    if img is None:
        return {"success": True, "error": "", "lines": []}

    rec_res, _elapsed = recognizer([img])
    lines = []
    for text, conf in rec_res:
        filtered = filter_digit_text(text)
        if filtered:
            lines.append({
                "text": filtered,
                "x1": 0,
                "y1": 0,
                "x2": 0,
                "y2": 0,
                "confidence": float(conf),
            })
    return {"success": True, "error": "", "lines": lines}


def run_on_image(ocr, lang: str, image_path: str, *, digits_only: bool = False) -> dict:
    if digits_only:
        return run_digits_only(lang, image_path)
    result = ocr.ocr(image_path, cls=True)
    lines = parse_result(result)
    return {"success": True, "error": "", "lines": lines}


def parse_request(text: str) -> tuple[str, bool]:
    text = text.strip()
    if text.startswith("{"):
        try:
            req = json.loads(text)
            path = req.get("image") or req.get("path") or ""
            return str(path), bool(req.get("digits_only"))
        except json.JSONDecodeError:
            pass
    return text, False


def serve_mode(lang: str) -> int:
    try:
        ocr = create_ocr(lang)
    except ImportError:
        emit_json({
            "ready": True,
            "success": False,
            "error": "paddleocr not installed. Run: pip install -r tools/requirements-ocr.txt",
            "lines": [],
        })
        return 1
    except Exception as exc:  # pylint: disable=broad-except
        emit_json({"ready": True, "success": False, "error": str(exc), "lines": []})
        return 1

    emit_json({"ready": True, "success": True, "error": "", "lines": []})

    while True:
        raw = sys.stdin.buffer.readline()
        if not raw:
            break
        text = raw.decode("utf-8", errors="replace").strip()
        if not text or text.upper() == "QUIT":
            break
        image_path, digits_only = parse_request(text)
        try:
            emit_json(run_on_image(ocr, lang, image_path, digits_only=digits_only))
        except Exception as exc:  # pylint: disable=broad-except
            emit_json({"success": False, "error": str(exc), "lines": []})
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run PaddleOCR on an image file.")
    parser.add_argument("--image", help="Path to input image (bmp/png/jpg)")
    parser.add_argument("--lang", default="ch", help="OCR language, default: ch")
    parser.add_argument("--digits-only", action="store_true", help="Digit-only rec OCR")
    parser.add_argument("--serve", action="store_true", help="Keep process alive for multiple requests via stdin")
    args = parser.parse_args()

    if args.serve:
        return serve_mode(args.lang)

    if not args.image:
        emit_json({"success": False, "error": "missing --image", "lines": []})
        return 1

    try:
        ocr = create_ocr(args.lang)
        emit_json(run_on_image(ocr, args.lang, args.image, digits_only=args.digits_only))
    except ImportError:
        emit_json({
            "success": False,
            "error": "paddleocr not installed. Run: pip install -r tools/requirements-ocr.txt",
            "lines": [],
        })
        return 1
    except Exception as exc:  # pylint: disable=broad-except
        emit_json({"success": False, "error": str(exc), "lines": []})
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
