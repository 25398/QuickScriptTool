#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PaddleOCR helper — called by QuickScriptTool C++ OCR engine."""

import argparse
import json
import os
import sys

# Avoid Unicode user-profile paths (Paddle Inference cannot open models there on Windows).
MODEL_ROOT = r"C:\paddle_env\models\whl"


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

    for page in result:
        if not page:
            continue
        for block in page:
            if not block or len(block) < 2:
                continue
            box = block[0]
            text_info = block[1]
            if not box or not text_info:
                continue
            text = text_info[0] if text_info else ""
            conf = float(text_info[1]) if len(text_info) > 1 else 0.0
            if not text:
                continue
            xs = [float(p[0]) for p in box]
            ys = [float(p[1]) for p in box]
            lines.append({
                "text": text,
                "x1": int(min(xs)),
                "y1": int(min(ys)),
                "x2": int(max(xs)),
                "y2": int(max(ys)),
                "confidence": conf,
            })
    return lines


def emit_json(payload: dict) -> None:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


def create_ocr(lang: str):
    os.environ["PADDLEOCR_HOME"] = r"C:\paddle_env\models"
    from paddleocr import PaddleOCR

    return PaddleOCR(
        use_angle_cls=True,
        lang=lang,
        show_log=False,
        **model_dirs(lang),
    )


def run_on_image(ocr, image_path: str) -> dict:
    result = ocr.ocr(image_path, cls=True)
    lines = parse_result(result)
    return {"success": True, "error": "", "lines": lines}


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
        try:
            emit_json(run_on_image(ocr, text))
        except Exception as exc:  # pylint: disable=broad-except
            emit_json({"success": False, "error": str(exc), "lines": []})
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run PaddleOCR on an image file.")
    parser.add_argument("--image", help="Path to input image (bmp/png/jpg)")
    parser.add_argument("--lang", default="ch", help="OCR language, default: ch")
    parser.add_argument("--serve", action="store_true", help="Keep process alive for multiple requests via stdin")
    args = parser.parse_args()

    if args.serve:
        return serve_mode(args.lang)

    if not args.image:
        emit_json({"success": False, "error": "missing --image", "lines": []})
        return 1

    try:
        ocr = create_ocr(args.lang)
        emit_json(run_on_image(ocr, args.image))
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
