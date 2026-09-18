#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OCR helper for QuickScriptTool — RapidOCR (ONNX) with PaddleOCR fallback."""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
from typing import Any

DIGIT_CHARSET = set("0123456789.,/-%+:$")
MIN_UPSCALE_SIDE = 56
MAX_OCR_SIDE = 1920
LOW_CONTRAST_STD = 28.0
# Cap 4K+ so det stays near PP-OCR's 960 long-side. Do not shrink 1080p to 640:
# 12–16px HUD digits fall below DBNet and were dropped by a 6px crop floor.
DIGIT_DET_MAX_SIDE = 1920
DIGIT_DET_BOX_THRESH = 0.35
DIGIT_DET_UNCLIP = 2.2
DIGIT_REC_MIN_CONF = 0.45

_out = getattr(sys.stdout, "buffer", sys.stdout)


def apply_compat_shim() -> None:
    """Keep PaddleOCR 2.x importable as a fallback on Python 3.10+."""
    import collections
    import collections.abc

    os.environ.setdefault("KMP_DUPLICATE_LIB_OK", "TRUE")
    os.environ.setdefault("PADDLEOCR_HOME", r"C:\paddle_env\models")
    for name in (
        "Mapping",
        "MutableMapping",
        "Sequence",
        "Callable",
        "Iterable",
        "MutableSequence",
    ):
        if not hasattr(collections, name) and hasattr(collections.abc, name):
            setattr(collections, name, getattr(collections.abc, name))


apply_compat_shim()


def guard_stdout() -> None:
    """RapidOCR/Paddle log to stdout and would break the JSON protocol."""
    global _out
    try:
        sys.stdout.flush()
    except Exception:
        pass
    _out = sys.stdout.buffer
    sys.stdout = open(os.devnull, "w", encoding="utf-8", errors="replace")


def emit_json(payload: dict) -> None:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    _out.write(data)
    _out.write(b"\n")
    _out.flush()


def imread_unicode(path: str):
    import cv2
    import numpy as np

    if not path:
        return None
    try:
        data = np.fromfile(path, dtype=np.uint8)
    except OSError:
        return None
    if data.size == 0:
        return None
    return cv2.imdecode(data, cv2.IMREAD_COLOR)


def decode_image_b64(b64: str):
    import cv2
    import numpy as np

    if not b64:
        return None
    try:
        raw = base64.b64decode(b64, validate=False)
    except Exception:
        return None
    if not raw:
        return None
    arr = np.frombuffer(raw, dtype=np.uint8)
    return cv2.imdecode(arr, cv2.IMREAD_COLOR)


def add_line(lines: list[dict], text, conf, box=None) -> None:
    if not text:
        return
    x1 = y1 = x2 = y2 = 0
    if box is not None:
        try:
            xs = [float(p[0]) for p in box]
            ys = [float(p[1]) for p in box]
            x1, y1, x2, y2 = int(min(xs)), int(min(ys)), int(max(xs)), int(max(ys))
        except (TypeError, ValueError, IndexError):
            pass
    lines.append({
        "text": str(text),
        "x1": x1,
        "y1": y1,
        "x2": x2,
        "y2": y2,
        "confidence": float(conf) if conf is not None else 0.0,
    })


def parse_paddle_result(result) -> list[dict]:
    lines: list[dict] = []
    if not result:
        return lines
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
                    add_line(lines, text_info[0], text_info[1] if len(text_info) > 1 else 0.0, box)
            elif isinstance(block, (list, tuple)) and len(block) >= 2 and isinstance(block[0], str):
                add_line(lines, block[0], block[1] if len(block) > 1 else 0.0)
            elif isinstance(block, (list, tuple)) and len(block) >= 1:
                inner = block[0]
                if isinstance(inner, (list, tuple)) and len(inner) >= 2 and isinstance(inner[0], str):
                    add_line(lines, inner[0], inner[1] if len(inner) > 1 else 0.0)
    return lines


def parse_rapid_result(result) -> list[dict]:
    lines: list[dict] = []
    if result is None:
        return lines

    boxes = getattr(result, "boxes", None)
    txts = getattr(result, "txts", None)
    scores = getattr(result, "scores", None)
    if txts is not None:
        n = len(txts)
        for i in range(n):
            raw = txts[i]
            text = raw
            conf = 0.0
            if isinstance(raw, (list, tuple)) and raw:
                text = raw[0]
                if len(raw) > 1:
                    try:
                        conf = float(raw[1])
                    except (TypeError, ValueError):
                        conf = 0.0
            elif isinstance(scores, (int, float)):
                if i == 0:
                    conf = float(scores)
            elif scores is not None:
                try:
                    conf = float(scores[i])
                except (TypeError, ValueError, IndexError):
                    conf = 0.0
            box = None
            if boxes is not None:
                try:
                    box = boxes[i]
                except (TypeError, IndexError):
                    box = None
            add_line(lines, text, conf, box)
        return lines

    # Older rapidocr: (list of [box, text, score], elapse)
    payload = result
    if isinstance(result, (list, tuple)) and result and not isinstance(result[0], dict):
        if len(result) >= 1 and isinstance(result[0], (list, tuple)):
            payload = result[0]
    if not payload:
        return lines
    for item in payload:
        if item is None:
            continue
        if isinstance(item, (list, tuple)) and len(item) >= 2:
            box = item[0] if not isinstance(item[0], str) else None
            if isinstance(item[0], str):
                add_line(lines, item[0], item[1] if len(item) > 1 else 0.0)
            else:
                text = item[1]
                conf = item[2] if len(item) > 2 else 0.0
                if isinstance(text, (list, tuple)):
                    conf = text[1] if len(text) > 1 else 0.0
                    text = text[0]
                add_line(lines, text, conf, box)
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


def scale_lines(lines: list[dict], scale: float) -> list[dict]:
    if not scale or abs(scale - 1.0) < 1e-6:
        return lines
    inv = 1.0 / scale
    out = []
    for line in lines:
        entry = dict(line)
        entry["x1"] = int(round(entry.get("x1", 0) * inv))
        entry["y1"] = int(round(entry.get("y1", 0) * inv))
        entry["x2"] = int(round(entry.get("x2", 0) * inv))
        entry["y2"] = int(round(entry.get("y2", 0) * inv))
        out.append(entry)
    return out


def preprocess_screen_image(img):
    """Upscale tiny UI text; downscale huge captures; CLAHE only on low-contrast crops."""
    import cv2
    import numpy as np

    if img is None or img.size == 0:
        return img, 1.0
    h, w = img.shape[:2]
    if h <= 0 or w <= 0:
        return img, 1.0

    scale = 1.0
    min_side = min(h, w)
    max_side = max(h, w)
    if max_side > MAX_OCR_SIDE:
        scale = MAX_OCR_SIDE / float(max_side)
        img = cv2.resize(
            img,
            (max(1, int(round(w * scale))), max(1, int(round(h * scale)))),
            interpolation=cv2.INTER_AREA,
        )
    elif min_side < MIN_UPSCALE_SIDE and max_side < 1600:
        scale = min(3.0, MIN_UPSCALE_SIDE / float(max(min_side, 1)))
        img = cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_CUBIC)

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    if float(np.std(gray)) < LOW_CONTRAST_STD:
        clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
        enhanced = clahe.apply(gray)
        img = cv2.cvtColor(enhanced, cv2.COLOR_GRAY2BGR)
    return img, scale


def preprocess_digit_crop(img):
    """Keep aspect. Tiny HUD numbers upscale; huge captures downscale. Rec owns its own input shape."""
    prepared, _scale = preprocess_screen_image(img)
    return prepared


def stamp_full_crop_box(lines: list[dict], crop_w: int, crop_h: int) -> list[dict]:
    for line in lines:
        line["x1"] = 0
        line["y1"] = 0
        line["x2"] = max(1, crop_w)
        line["y2"] = max(1, crop_h)
    return lines


def create_rapidocr():
    from rapidocr import RapidOCR

    cpu = os.cpu_count() or 4
    threads = max(1, min(4, cpu))
    params = {
        "EngineConfig.onnxruntime.intra_op_num_threads": threads,
        "EngineConfig.onnxruntime.inter_op_num_threads": 1,
        "EngineConfig.onnxruntime.enable_cpu_mem_arena": False,
    }
    try:
        return RapidOCR(params=params)
    except TypeError:
        return RapidOCR()


def create_paddleocr(lang: str):
    os.environ["PADDLEOCR_HOME"] = r"C:\paddle_env\models"
    from paddleocr import PaddleOCR

    cpu = os.cpu_count() or 4
    model_root = r"C:\paddle_env\models\whl"
    dirs = {}
    det = os.path.join(model_root, "det", "ch", "ch_PP-OCRv4_det_infer")
    rec = os.path.join(model_root, "rec", "ch", "ch_PP-OCRv4_rec_infer")
    cls = os.path.join(model_root, "cls", "ch_ppocr_mobile_v2.0_cls_infer")
    if os.path.isfile(os.path.join(det, "inference.pdmodel")):
        dirs["det_model_dir"] = det
    if os.path.isfile(os.path.join(rec, "inference.pdmodel")):
        dirs["rec_model_dir"] = rec
    if os.path.isfile(os.path.join(cls, "inference.pdmodel")):
        dirs["cls_model_dir"] = cls
    base = {
        "use_angle_cls": True,
        "lang": lang,
        "show_log": False,
        "use_mkldnn": True,
        "cpu_threads": max(1, min(4, cpu)),
        **dirs,
    }
    try:
        return PaddleOCR(**base)
    except Exception:
        base.pop("use_mkldnn", None)
        return PaddleOCR(**base)


def create_engine(lang: str) -> tuple[Any, str]:
    try:
        return create_rapidocr(), "rapidocr"
    except Exception:
        pass
    try:
        return create_paddleocr(lang), "paddleocr"
    except ImportError as exc:
        raise ImportError(
            "rapidocr not installed. Run: pip install -r tools/requirements-ocr.txt"
        ) from exc


def shrink_max_side(img, max_side: int):
    import cv2

    h, w = img.shape[:2]
    longest = max(h, w)
    if longest <= max_side:
        return img, 1.0
    scale = max_side / float(longest)
    new_w = max(1, int(round(w * scale)))
    new_h = max(1, int(round(h * scale)))
    return cv2.resize(img, (new_w, new_h), interpolation=cv2.INTER_AREA), scale


def create_paddle_en_recognizer():
    """Official Paddle English+digit rec head (en_PP-OCRv4). Rec only, no extra det."""
    os.environ["PADDLEOCR_HOME"] = r"C:\paddle_env\models"
    from pathlib import Path

    import paddleocr
    from paddleocr.paddleocr import (
        BASE_DIR,
        confirm_model_dir_url,
        get_model_config,
        parse_args,
    )
    from paddleocr.ppocr.utils.network import maybe_download
    from paddleocr.tools.infer.predict_rec import TextRecognizer

    params = parse_args(mMain=False)
    params.lang = "en"
    params.show_log = False
    params.use_gpu = False
    params.enable_mkldnn = True
    params.cpu_threads = max(1, min(4, os.cpu_count() or 4))
    params.ocr_version = "PP-OCRv4"
    params.rec_image_shape = "3, 48, 320"
    rec_cfg = get_model_config("OCR", "PP-OCRv4", "rec", "en")
    params.rec_model_dir, rec_url = confirm_model_dir_url(
        None, os.path.join(BASE_DIR, "whl", "rec", "en"), rec_cfg["url"])
    maybe_download(params.rec_model_dir, rec_url)
    params.rec_char_dict_path = str(Path(paddleocr.__file__).parent / rec_cfg["dict_path"])
    try:
        return TextRecognizer(params)
    except Exception:
        params.enable_mkldnn = False
        return TextRecognizer(params)


def get_paddle_en_recognizer():
    cached = getattr(get_paddle_en_recognizer, "_rec", None)
    if cached is None:
        get_paddle_en_recognizer._rec = create_paddle_en_recognizer()
    return get_paddle_en_recognizer._rec


def _aabb_crop(img, box, pad_ratio: float = 0.22):
    """Crop with padding. Small HUD digits are often clipped by tight det boxes."""
    import numpy as np

    pts = np.asarray(box, dtype=np.float32).reshape(-1, 2)
    h, w = img.shape[:2]
    x1 = int(pts[:, 0].min())
    y1 = int(pts[:, 1].min())
    x2 = int(np.ceil(pts[:, 0].max()))
    y2 = int(np.ceil(pts[:, 1].max()))
    bw = max(1, x2 - x1)
    bh = max(1, y2 - y1)
    pad_x = max(3, int(round(bw * pad_ratio)))
    pad_y = max(4, int(round(bh * max(pad_ratio, 0.28))))
    x1 = max(0, x1 - pad_x)
    y1 = max(0, y1 - pad_y)
    x2 = min(w, x2 + pad_x)
    y2 = min(h, y2 + pad_y)
    if x2 - x1 < 3 or y2 - y1 < 3:
        return None, None
    return img[y1:y2, x1:x2].copy(), (x1, y1, x2, y2)


def _prepare_rec_crop(crop):
    import cv2

    h, w = crop.shape[:2]
    if h <= 0 or w <= 0:
        return crop
    if h < 28:
        scale = 32.0 / float(h)
        crop = cv2.resize(
            crop,
            (max(8, int(round(w * scale))), 32),
            interpolation=cv2.INTER_CUBIC,
        )
    return crop


def _det_postprocess(engine, name: str):
    if name == "rapidocr":
        det = getattr(engine, "text_det", None)
        if det is None and hasattr(engine, "_load_det_model"):
            det = engine._load_det_model()
        return getattr(det, "postprocess_op", None) if det is not None else None
    det = getattr(engine, "text_detector", None)
    return getattr(det, "postprocess_op", None) if det is not None else None


def _apply_digit_det_params(engine, name: str):
    op = _det_postprocess(engine, name)
    if op is None:
        return None
    old = (
        getattr(op, "box_thresh", None),
        getattr(op, "unclip_ratio", None),
    )
    if old[0] is not None:
        op.box_thresh = min(float(old[0]), DIGIT_DET_BOX_THRESH)
    if old[1] is not None:
        op.unclip_ratio = max(float(old[1]), DIGIT_DET_UNCLIP)
    return (op, old)


def _restore_digit_det_params(token) -> None:
    if not token:
        return
    op, old = token
    if old[0] is not None:
        op.box_thresh = old[0]
    if old[1] is not None:
        op.unclip_ratio = old[1]


def detect_boxes_only(engine, name: str, det_img) -> list:
    token = _apply_digit_det_params(engine, name)
    try:
        if name == "rapidocr":
            out = engine(det_img, use_det=True, use_cls=False, use_rec=False)
            boxes = getattr(out, "boxes", None)
            return list(boxes) if boxes is not None else []
        dt_boxes, _elapsed = engine.text_detector(det_img)
        if dt_boxes is None or len(dt_boxes) == 0:
            return []
        return list(dt_boxes)
    finally:
        _restore_digit_det_params(token)


def _parse_rec_pairs(rec_res) -> list[tuple[str, float]]:
    pairs: list[tuple[str, float]] = []
    for item in rec_res or []:
        if isinstance(item, (list, tuple)) and item:
            conf = item[1] if len(item) > 1 else 0.0
            try:
                conf = float(conf)
            except (TypeError, ValueError):
                conf = 0.0
            pairs.append((str(item[0]), conf))
        elif isinstance(item, str):
            pairs.append((item, 0.0))
    return pairs


def rec_digit_crops(engine, name: str, crops: list) -> list[tuple[str, float]]:
    """English/digit head first; retry weak/empty crops with the Chinese rec (game fonts)."""
    prepared = [_prepare_rec_crop(c) for c in crops]
    pairs: list[tuple[str, float]] = [("", 0.0)] * len(prepared)
    if not prepared:
        return pairs
    if name == "rapidocr":
        rec_res = _rapid_rec_only(engine, prepared)
        parsed = parse_rapid_result(rec_res)
        for i, line in enumerate(parsed):
            if i < len(pairs):
                pairs[i] = (line.get("text", ""), float(line.get("confidence") or 0.0))
        return pairs
    try:
        rec_res, _elapsed = get_paddle_en_recognizer()(prepared)
        pairs = _parse_rec_pairs(rec_res)
        if len(pairs) < len(prepared):
            pairs.extend([("", 0.0)] * (len(prepared) - len(pairs)))
    except Exception:
        rec_res, _elapsed = engine.text_recognizer(prepared)
        return _parse_rec_pairs(rec_res)
    retry_idx = []
    for i, (text, conf) in enumerate(pairs):
        if i >= len(crops):
            break
        if conf < DIGIT_REC_MIN_CONF:
            retry_idx.append(i)
            continue
        if not filter_digit_text(text):
            ch, cw = crops[i].shape[:2]
            if ch <= 42 and cw <= 180:
                retry_idx.append(i)
    if retry_idx:
        retry_crops = [prepared[i] for i in retry_idx]
        rec_res, _elapsed = engine.text_recognizer(retry_crops)
        retry_pairs = _parse_rec_pairs(rec_res)
        for j, i in enumerate(retry_idx):
            if j >= len(retry_pairs):
                break
            alt_text, alt_conf = retry_pairs[j]
            if filter_digit_text(alt_text) and (
                not filter_digit_text(pairs[i][0]) or alt_conf > pairs[i][1]
            ):
                pairs[i] = (alt_text, alt_conf)
    return pairs


def crop_is_single_line(img) -> bool:
    """Tight HUD / countdown can skip det. Full screen or a UI block must locate numbers first."""
    if img is None or img.size == 0:
        return False
    h, w = img.shape[:2]
    if h <= 0 or w <= 0:
        return False
    if h <= 72 and w <= 960:
        return True
    if h <= 96 and w <= 480:
        return True
    return False


def run_engine(engine, name: str, img, *, use_cls: bool = True) -> list[dict]:
    # RapidOCR.__call__ mutates use_det/use_cls when kwargs are set; always pass flags.
    if name == "rapidocr":
        result = engine(img, use_det=True, use_cls=use_cls, use_rec=True)
        return parse_rapid_result(result)
    result = engine.ocr(img, det=True, rec=True, cls=use_cls)
    return parse_paddle_result(result)


def run_digits_scene(engine, name: str, img) -> list[dict]:
    """Det at near-full resolution, rec original padded crops with en then Chinese fallback."""
    import numpy as np

    det_img, scale = shrink_max_side(img, DIGIT_DET_MAX_SIDE)
    boxes = detect_boxes_only(engine, name, det_img)
    inv = 1.0 / scale if scale else 1.0
    crops = []
    rects = []
    for box in boxes:
        try:
            mapped = np.asarray(box, dtype=np.float32) * inv
        except (TypeError, ValueError):
            continue
        crop, rect = _aabb_crop(img, mapped)
        if crop is None:
            continue
        crops.append(crop)
        rects.append(rect)
    if not crops:
        return []
    pairs = rec_digit_crops(engine, name, crops)
    lines: list[dict] = []
    for i, rect in enumerate(rects):
        text, conf = pairs[i] if i < len(pairs) else ("", 0.0)
        add_line(lines, text, conf, [
            (rect[0], rect[1]), (rect[2], rect[1]), (rect[2], rect[3]), (rect[0], rect[3]),
        ])
    return filter_digit_lines(lines)


def _rapid_rec_only(engine, img):
    """Call the rec session only. RapidOCR.__call__(use_det=False) is not rec-only:
    it still preprocess_img, mutates engine.use_det, and rec width follows the crop."""
    imgs = img if isinstance(img, list) else [img]
    rec_loader = getattr(engine, "_load_rec_model", None)
    if callable(rec_loader):
        rec_model = rec_loader()
        try:
            from rapidocr.ch_ppocr_rec import TextRecInput
            return rec_model(TextRecInput(img=imgs, return_word_box=False))
        except Exception:
            rec_in = type("RecIn", (), {})()
            rec_in.img = imgs
            rec_in.return_word_box = False
            return rec_model(rec_in)
    return engine.recognize_txt(imgs)


def run_rec_only(engine, name: str, lang: str, img, crop_w: int, crop_h: int) -> list[dict]:
    """Whole crop as one line. Reuse the live engine's rec."""
    lines: list[dict] = []
    if name == "rapidocr":
        try:
            rec_res = _rapid_rec_only(engine, img)
        except Exception:
            rec_res = None
        if rec_res is not None:
            lines = parse_rapid_result(rec_res)
    else:
        rec_ok = False
        try:
            result = engine.ocr(img, det=False, rec=True, cls=False)
            lines = parse_paddle_result(result)
            rec_ok = True
        except TypeError:
            rec_ok = False
        if not rec_ok:
            rec_res, _elapsed = engine.text_recognizer([img])
            for item in rec_res or []:
                if isinstance(item, (list, tuple)) and item:
                    text = item[0]
                    conf = item[1] if len(item) > 1 else 0.0
                    add_line(lines, text, conf)
    return stamp_full_crop_box(filter_digit_lines(lines), crop_w, crop_h)


def run_on_array(engine, name: str, img, *, digits_only: bool, lang: str = "ch") -> dict:
    if img is None:
        return {"success": True, "error": "", "lines": []}
    h, w = img.shape[:2]
    if digits_only and crop_is_single_line(img):
        prepared = preprocess_digit_crop(img)
        lines = run_rec_only(engine, name, lang, prepared, w, h)
        return {"success": True, "error": "", "lines": lines}
    if digits_only:
        prepared, scale = preprocess_screen_image(img)
        lines = scale_lines(run_digits_scene(engine, name, prepared), scale)
        return {"success": True, "error": "", "lines": lines}
    prepared, scale = preprocess_screen_image(img)
    lines = scale_lines(run_engine(engine, name, prepared, use_cls=True), scale)
    return {"success": True, "error": "", "lines": lines}


def load_request_image(path: str, image_b64: str):
    if image_b64:
        img = decode_image_b64(image_b64)
        if img is not None:
            return img
    if path:
        return imread_unicode(path)
    return None


def parse_request(text: str) -> tuple[str, bool, str]:
    text = text.strip()
    if text.startswith("{"):
        try:
            req = json.loads(text)
            path = str(req.get("image") or req.get("path") or "")
            b64 = str(req.get("image_b64") or req.get("image_base64") or "")
            return path, bool(req.get("digits_only")), b64
        except json.JSONDecodeError:
            pass
    return text, False, ""


def serve_mode(lang: str) -> int:
    guard_stdout()
    try:
        engine, name = create_engine(lang)
    except ImportError:
        emit_json({
            "ready": True,
            "success": False,
            "error": "rapidocr not installed. Run: pip install -r tools/requirements-ocr.txt",
            "lines": [],
            "engine": "",
        })
        return 1
    except Exception as exc:  # pylint: disable=broad-except
        emit_json({"ready": True, "success": False, "error": str(exc), "lines": [], "engine": ""})
        return 1

    emit_json({"ready": True, "success": True, "error": "", "lines": [], "engine": name})

    while True:
        raw = sys.stdin.buffer.readline()
        if not raw:
            break
        text = raw.decode("utf-8", errors="replace").strip()
        if not text or text.upper() == "QUIT":
            break
        path, digits_only, image_b64 = parse_request(text)
        try:
            img = load_request_image(path, image_b64)
            emit_json(run_on_array(engine, name, img, digits_only=digits_only, lang=lang))
        except Exception as exc:  # pylint: disable=broad-except
            emit_json({"success": False, "error": str(exc), "lines": []})
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run OCR on an image file or stdin JSON.")
    parser.add_argument("--image", help="Path to input image (png/jpg/bmp)")
    parser.add_argument("--lang", default="ch", help="OCR language, default: ch")
    parser.add_argument("--digits-only", action="store_true",
                        help="Skip det/cls; recognize the whole crop as one line (digit charset)")
    parser.add_argument("--serve", action="store_true", help="Keep process alive for stdin JSON requests")
    args = parser.parse_args()

    if args.serve:
        return serve_mode(args.lang)

    guard_stdout()
    if not args.image:
        emit_json({"success": False, "error": "missing --image", "lines": []})
        return 1

    try:
        engine, name = create_engine(args.lang)
        img = load_request_image(args.image, "")
        emit_json(run_on_array(engine, name, img, digits_only=args.digits_only, lang=args.lang))
    except ImportError:
        emit_json({
            "success": False,
            "error": "rapidocr not installed. Run: pip install -r tools/requirements-ocr.txt",
            "lines": [],
        })
        return 1
    except Exception as exc:  # pylint: disable=broad-except
        emit_json({"success": False, "error": str(exc), "lines": []})
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
