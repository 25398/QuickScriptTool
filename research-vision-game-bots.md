# Open-Source Vision-Based Game Bots / Screen-Reading Game Agents — Research Dump

For an engineering report. Every claim carries a source URL. Anything not confirmed against a real page is
labelled **UNVERIFIED**. Star counts / dates come from `api.github.com/repos/<owner>/<repo>` where the
unauthenticated quota allowed it, otherwise from shields.io badges (**rounded**) or a GitHub search-API object;
the distinction is noted where it matters. GitHub's shared-IP API quota was exhausted partway through this
research, so several lookups returned **HTTP 403 (rate limit), not 404** — those are flagged as
"metadata UNVERIFIED", never as "does not exist".

---

## 0. Corrections to the research brief (all verified)

1. **`cv2.matchTemplate`'s `mask` parameter dates to OpenCV 3.0.0 — not 4.x.** `matchTemplateMask()` is absent
   from 2.4.x and from `3.0.0-beta`, and present from `3.0.0-rc1` onward.
   <https://raw.githubusercontent.com/opencv/opencv/2.4.13.7/modules/imgproc/src/templmatch.cpp> ·
   <https://raw.githubusercontent.com/opencv/opencv/3.0.0-beta/modules/imgproc/src/templmatch.cpp> ·
   <https://raw.githubusercontent.com/opencv/opencv/3.0.0-rc1/modules/imgproc/src/templmatch.cpp>
   The signature `matchTemplate(image, templ, result, method, mask = noArray())` is identical in the 3.0.0,
   3.4.0, 4.0.0 and 4.x headers. The exact 3.0.0-cycle PR/commit is **UNVERIFIED** (commit API was 403).

2. **But the brief's "TM_SQDIFF and TM_CCORR_NORMED only" is correct for a real, wide version range — and is
   obsolete from OpenCV 4.4.0.** Verified by reading `matchTemplateMask()` at each tag:
   | Version | Masked methods implemented | Fallback |
   |---|---|---|
   | 2.4.x, 3.0.0-beta | none (no `mask`) | — |
   | 3.0.0-rc1 … 4.3.0 (incl. current `3.4` branch) | **`TM_SQDIFF`, `TM_CCORR_NORMED`** | `CV_Error(Error::StsNotImplemented, "")` |
   | **4.4.0 → current 4.x** | **all six** | none |
   URLs: <https://raw.githubusercontent.com/opencv/opencv/4.0.0/modules/imgproc/src/templmatch.cpp> ·
   <https://raw.githubusercontent.com/opencv/opencv/4.3.0/modules/imgproc/src/templmatch.cpp> ·
   <https://raw.githubusercontent.com/opencv/opencv/4.4.0/modules/imgproc/src/templmatch.cpp> ·
   <https://raw.githubusercontent.com/opencv/opencv/4.5.0/modules/imgproc/src/templmatch.cpp> ·
   <https://raw.githubusercontent.com/opencv/opencv/4.x/modules/imgproc/src/templmatch.cpp>
   **The introducing PR number is UNVERIFIED.**

3. **The belief in (2) is manufactured by a stale sentence that OpenCV still ships.** The current **4.x C++
   tutorial** says, verbatim: *"**Only two matching methods currently accept a mask: TM_SQDIFF and
   TM_CCORR_NORMED**"* — false since 4.4.0.
   <https://raw.githubusercontent.com/opencv/opencv/4.x/doc/tutorials/imgproc/histograms/template_matching/template_matching.markdown>
   (same sentence in 4.5.4). The **Python tutorial never mentions the mask at all** (grep for `mask` → zero
   hits): <https://raw.githubusercontent.com/opencv/opencv/4.x/doc/py_tutorials/py_imgproc/py_template_matching/py_template_matching.markdown>.
   A forum report flags exactly this contradiction:
   <https://forum.opencv.org/t/missing-the-cost-function-of-tm-ccoeff-normed-with-mask-for-matchtemplate-in-api-doc/6481>
   Also: **`github.com/opencv/opencv/pull/11888` is a red herring** — it is *"Error in Matx::solve for
   non-square matrices"*, unrelated to matchTemplate. <https://github.com/opencv/opencv/issues/11888>

4. **`Farama-Foundation/Gym-Retro` does not exist.** `https://github.com/Farama-Foundation/Gym-Retro` and
   `https://api.github.com/repos/Farama-Foundation/Gym-Retro` both return **Not Found**. `openai/gym-retro`
   is also 404 — **`gym-retro` is only a PyPI distribution name for `openai/retro`**
   (<https://pypi.org/project/gym-retro/> — 0.8.0, 2020-05-01, MIT, homepage `https://github.com/openai/retro`).
   The real Farama successor is **`Farama-Foundation/stable-retro`**:
   <https://github.com/Farama-Foundation/stable-retro> · docs <https://stable-retro.farama.org/>.

5. **Sentdex's ROI function is named `roi(img, vertices)`**, not `region_of_interest()` — the latter is a
   downstream-fork convention. <https://pythonprogramming.net/lane-region-of-interest-python-plays-gta-v/>

6. **`Sentdex/pygta5` master is no longer the 2017 screen-capture project.** Master holds a 2022 reboot
   (in-game mod + `vgamepad`). The 2017 GDI/`SendInput` code is under `original_project/`, whose README says:
   *"NOTE: This old project is provided in legacy mode. Currently there are no plans to provide any updates to
   it."* <https://raw.githubusercontent.com/Sentdex/pygta5/master/original_project/README.md>

7. **Non-vision projects to exclude (confirmed):** `poke-env` = *"Python Interface for **Pokemon Showdown**
   Bots"* (protocol, not CV) <https://github.com/hsahovic/poke-env>; `pret/pokeruby` = *"**Decompilation** of
   Pokémon Ruby/Sapphire"* <https://github.com/pret/pokeruby>; `ob-f/OpenBot` = smartphone-brained
   **robotics** kit, not a game bot <https://github.com/ob-f/OpenBot>.
   `peterfredholm/PokemonRedExperiments` **404 — does not exist**; the repo is `PWhiddy/PokemonRedExperiments`.
   **`WoW-5.4.8-OpenBot`: NOT FOUND anywhere — do not cite.**
   `tmrlproj/tmrl` **404 — does not exist**; the repo is **`trackmania-rl/tmrl`**.

8. **pyautogui's docs never call `confidence` "unreliable"** — they call the locate functions *slow*. If the
   report wants "unreliable", it must cite the issue tracker, not the docs (§7a).

---

## 1. Sentdex `pygta5` / `pythonplaysgta`

**Repo**: <https://github.com/Sentdex/pygta5> — **3,905★**, 803 forks, MIT, created 2017-04-05,
`pushed_at` **2023-03-08**, `archived: false`, default branch `master`.
Metadata: <https://api.github.com/repos/Sentdex/pygta5>

### 1a. The 2017 original — screen-capture lane keeper (`original_project/`)

**Perception loop**
- **Capture API: Windows GDI `BitBlt` of the desktop window** (not DXGI, not `mss`):
  `win32gui.GetDesktopWindow()` → `GetWindowDC` → `CreateCompatibleDC` / `CreateCompatibleBitmap` →
  `memdc.BitBlt((0,0), (width,height), srcdc, (left,top), win32con.SRCCOPY)` → `bmp.GetBitmapBits(True)` →
  `np.fromstring(..., dtype='uint8').shape = (height,width,4)` → `cv2.cvtColor(img, cv2.COLOR_BGRA2RGB)`.
  Optional `region=(left,top,x2,y2)`; **without a region it captures the whole virtual desktop** via
  `SM_CXVIRTUALSCREEN`/`SM_CYVIRTUALSCREEN`. File credited to "Frannecklp".
  <https://raw.githubusercontent.com/Sentdex/pygta5/master/original_project/grabscreen.py>
- The lane keeper used **PIL `ImageGrab`, not GDI**: `np.array(ImageGrab.grab(bbox=(0,40,800,640)))` —
  an **800×600** region. Hard documented requirement: the game must be **windowed at 800×600, top-left of the
  screen** — *"Eventually we can go off the window's name, but, for now, the current code wants the window in
  the corner."* <https://raw.githubusercontent.com/Sentdex/pygta5/master/original_project/README.md>
- **Frame pacing: `cv2.waitKey(25)`** → nominal **~40 FPS** ceiling. Per-frame cost is *printed*, not enforced:
  `print('Frame took {} seconds'.format(time.time()-last_time))` and
  `print('Loop took {} seconds'.format(time.time()-last_time))`.
  <https://pythonprogramming.net/self-driving-car-python-plays-gta-v/> ·
  <https://pythonprogramming.net/lane-region-of-interest-python-plays-gta-v/>
- The CNN variant captured **`(0,40,1920,1120)`** and downscaled to **`(480,270)`**:
  `cv2.resize(screen, (480,270))`.
  <https://raw.githubusercontent.com/Sentdex/pygta5/master/original_project/1.%20collect_data.py>

**Detection (`process_img`) — exact code**
```python
def roi(img, vertices):
    mask = np.zeros_like(img)
    cv2.fillPoly(mask, vertices, 255)
    masked = cv2.bitwise_and(img, mask)
    return masked

def process_img(image):
    processed_img = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    processed_img = cv2.Canny(processed_img, threshold1=200, threshold2=300)
    processed_img = cv2.GaussianBlur(processed_img, (5,5), 0)
    vertices = np.array([[10,500],[10,300],[300,200],[500,200],[800,300],[800,500]], np.int32)
    processed_img = roi(processed_img, [vertices])
    #                                     rho   theta   thresh  min length, max gap:
    lines = cv2.HoughLinesP(processed_img, 1, np.pi/180, 180,      20,       15)
```
- Constants: **Canny 200/300**, **GaussianBlur (5,5)**, a 6-vertex trapezoid ROI, **HoughLinesP rho=1,
  theta=π/180, threshold=180**, then literals `20, 15`.
- **Ambiguity worth flagging as a code smell, not a spec**: `HoughLinesP(image, rho, theta, threshold, lines,
  minLineLength, maxLineGap)`. Passing `20, 15` positionally puts `20` in the `lines` slot and `15` in
  `minLineLength`; the inline comment (`min length, max gap`) states the author's *intent*. The effective
  minLineLength/maxLineGap in this snippet are therefore not what the comment claims.
- **Lane fitting**: per Hough segment, slope/intercept by least squares
  (`vstack([x_coords, ones(len(x_coords))]).T` + `numpy.linalg.lstsq`); segments are merged into the same lane
  when `abs(other_ms*1.2) > abs(m) > abs(other_ms*0.8)` (a **±20% slope band**, with the same test on the
  intercept); the **top-2 clusters by member count** are averaged into two lane lines; any exception → the
  slopes **default to 0**.
- **Steering rule (pure sign logic, no model)**: both slopes negative → `right()`; both positive → `left()`;
  otherwise `straight()`.
- Documented failure: *"It's not perfect, and sometimes it gets going too fast and runs out of the lane and
  gets lost... When it doesn't work, the lane finders goes nuts."*
  <https://pythonprogramming.net/self-driving-car-python-plays-gta-v/>

**Act output — `directkeys.py`, ctypes `SendInput` with scancodes**
- `SendInput = ctypes.windll.user32.SendInput`; hand-written `KeyBdInput` / `HardwareInput` / `MouseInput` /
  `Input_I` / `Input` structs.
- Scancodes: **`W=0x11, A=0x1E, S=0x1F, D=0x20`**, numpad **`NP_2=0x50, NP_4=0x4B, NP_6=0x4D, NP_8=0x48`**.
- `PressKey` → `dwFlags = 0x0008` (`KEYEVENTF_SCANCODE`); `ReleaseKey` → `0x0008 | 0x0002`
  (`SCANCODE | KEYUP`). This is the ancestor pattern many later bots copy.
  <https://raw.githubusercontent.com/Sentdex/pygta5/master/original_project/directkeys.py>
- Input *reading* for labels: `win32api.GetAsyncKeyState(ord(key))` polled over a fixed `keyList`.
  <https://raw.githubusercontent.com/Sentdex/pygta5/master/original_project/getkeys.py>

**Robustness tricks actually present**
- **Motion / stuck detection** — `delta_images` = `cv2.absdiff(t2, t0)`; `motion_detection` thresholds at **16**,
  `cv2.normalize(..., NORM_MINMAX)`, converts to gray, then `cv2.countNonZero`.
  <https://raw.githubusercontent.com/Sentdex/pygta5/master/original_project/motion.py>
- **Confirm-before-act via an N-consecutive-frames window**: `if motion_avg < motion_req and len(motion_log) >=
  log_len:` → *"WERE PROBABLY STUCK FFS, initiating some evasive maneuvers."* → random 4-way escape
  (reverse + turn) with `time.sleep(random.uniform(1,2))` between phases.
  Constants: **`motion_req = 800`**, **`motion_log = deque(maxlen=25)`**, `log_len = 25`, `how_far_remove = 800`.
- **Action blending / humanization**: in `left()`/`right()`, `random.randrange(0,3) == 1` decides whether to
  *also* hold `W`. Every call explicitly releases the other keys (idempotent key state, not delta state).
- **Prediction re-weighting before argmax**: raw 9-way output × `[4.5, 0.1, 0.1, 0.1, 1.8, 1.8, 0.5, 0.5, 0.2]`.
- **Input blur**: `t_plus = cv2.blur(t_plus, (4,4))`. Other constants: `choices = deque([], maxlen=5)`,
  `hl_hist = 250`, `t_time = 0.25`, `rs = (20,15)`, `WIDTH=480/HEIGHT=270`, `inception_v3(output=9)`.
  <https://raw.githubusercontent.com/Sentdex/pygta5/master/original_project/3.%20test_model.py>
- **Colorspace quirk**: `grabscreen.py` already returns RGB (`COLOR_BGRA2RGB`), yet both `1. collect_data.py`
  and `3. test_model.py` apply `cv2.cvtColor(screen, cv2.COLOR_BGR2RGB)` **again**. The double conversion is
  consistent between training and inference (so there is no train/test skew), but the model is in fact fed
  BGR-ordered data.
- Labels are a **9-class multi-hot** vector `[W, S, A, D, WA, WD, SA, SD, NOKEY]`; training data saved as
  `.npy` shards of **500 samples**.

### 1b. The 2022 reboot (current master) — a different architecture

- **No screen capture and no `SendInput`.** A **custom in-game GTA5 mod** grabs and resizes the frame; the
  model's outputs (acceleration/braking/steering) become **virtual controller inputs via `vgamepad`**:
  *"we have to translate predictions to controller inputs because of so-called dead zones in the controller used
  by GTA5."* <https://raw.githubusercontent.com/Sentdex/pygta5/master/project_info/system.md>
- **Frame rates**: the player renders at **90 FPS**, cycling three cameras so each renders at an effective
  **30 FPS** — *"the desired FPS for the project"*. All cameras render **1280×720**; the **Hood Camera (the
  model input) is downscaled to 480×270**.
  <https://raw.githubusercontent.com/Sentdex/pygta5/master/project_info/cameras.md>
- Data is sampled *"exactly every 1/30th of the second"*; the Player infers *"ideally 30 times a second"*.
  The console exposes **FPS** (hood-camera FPS) and **PPS** (predictions/second) as first-class metrics.
- 3–5 data-collector instances; backbones Xception / InceptionResNetV2 / RegNet.
  <https://raw.githubusercontent.com/Sentdex/pygta5/master/README.md>

---

## 2. SerpentAI

**Repo**: <https://github.com/SerpentAI/SerpentAI> — **6,990★**, 798 forks, MIT, created 2017-04-16,
`pushed_at` **2022-11-07**, **`archived: true`**, `has_issues: false`, default branch **`dev`**.
Metadata: <https://api.github.com/repos/SerpentAI/SerpentAI>

**Status / maintenance (documented in-repo)**
- README carries a struck-through *"Warning: End of life (November 2018)"* followed by *"Update: Revival
  (May 2020) — Development work has resumed... Python 3.8+, Less Dependencies, Ease of Use (Installer, GUI)"*.
  Badges: `pypi v2018.1.2`, `python 3.6`. **macOS support dropped** — *"Apple's aversion to Nvidia... means no
  recent macOS machine can run CUDA, an essential piece of technology for Serpent.AI's real-time training."*
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/README.md>
- `CHANGELOG.md` **stops at 2018.1.2** — nothing for 2019–2022.
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/CHANGELOG.md>
- GitHub reports the repo **archived**; last push 2022-11-07.
- Wiki lists sample plugins: Binding of Isaac: Afterbirth+, You Must Build a Boat, OpenRCT2 (WIP).
  <https://raw.githubusercontent.com/wiki/SerpentAI/SerpentAI/Home.md>

### 2a. Perception loop

- **Capture API: `mss`**, in a **separate process** (`serpent grab_frames W H x y [pipeline]`).
  `self.screen_grabber = mss.mss()`; `grab({"top","left","width","height"})` → `np.array(..., dtype="uint8")`
  → channel reorder `frame[..., [2,1,0,3]]` then `frame[..., :3]` (BGRA→RGB).
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/frame_grabber.py>
- **IPC**: frames go into a **Redis list** (`SERPENT:FRAMES` plus a parallel `_PIPELINE` list), `ltrim`-ed to
  `frame_buffer_size = buffer_seconds * fps` (defaults `buffer_seconds=5`, `fps=30` → **150 frames**).
- **Documented warm-up/back-pressure behaviour** — `FrameGrabber.get_frames()` **blocks until the Redis list
  length exceeds 149**, polling every 0.1 s:
  ```python
  while True:
      if redis_client.llen(config["frame_grabber"]["redis_key"]) > 149: break
      time.sleep(0.1)
  ```
  A consumer cannot obtain a frame until ~150 frames (~5 s at 30 fps) have accumulated. Same in
  `get_frames_with_pipeline()`.
- **Frame rate**: `GameFrameLimiter(fps=30)`. The base agent FPS was set to 30 in 0.1.3b1 — *"TWEAK - Base Game
  Agent GameFrameLimiter FPS to 30"*; `serpent/game.py` reads it per game plugin:
  `self.game_frame_limiter = GameFrameLimiter(fps=self.config.get("fps", 30))`.
  **Recording is throttled to 10 FPS**: `if frame_handler == "RECORD":
  self.game_frame_limiter = GameFrameLimiter(fps=10)`.
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/game.py>
- **Perception→act loop** (`Game.play`): `game_frame_limiter.start()` → `grab_latest_frame()` (index `[0]` of the
  Redis lists) → **`if self.is_focused:`** `game_agent.on_game_frame(...)` **`else:` `print("PAUSED")` and
  `time.sleep(1)`** → `game_frame_limiter.stop_and_delay()`. Focus-gating is first-class in the loop.
- `game_frame_limiter.py` is tiny and exact: `frame_time = 1/fps`; `stop_and_delay()` measures elapsed µs and
  sleeps the remainder. It uses `datetime.utcnow().microseconds` (the sub-second field only) — a latent
  precision bug for cycles longer than 1 s.
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/game_frame_limiter.py>

### 2b. `GameFrame` — the frame abstraction (the most copyable idea in the framework)

<https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/game_frame.py>

- Holds `frame_bytes` or `frame_array`; `offset_x`/`offset_y` for sub-region frames; **`resize_order` default 1**
  (bilinear; `skimage.transform.resize(..., mode="reflect", order=...)`).
- **Frame variants — lazily evaluated and cached** (a deliberate, documented design choice):
  | Variant | Size |
  |---|---|
  | `frame` | full resolution |
  | `half_resolution_frame` | w/2, h/2 |
  | `quarter_resolution_frame` | w/4, h/4 |
  | `eighth_resolution_frame` | w/8, h/8 |
  | `eighth_resolution_grayscale_frame` | w/8, h/8, gray |
  | `grayscale_frame` | full-res gray |
  | `ssim_frame` | **100×100 grayscale, `order=0`** (nearest) for SSIM |
- Wiki rationale, verbatim: *"For performance reasons, Frame Variants are not precomputed but rather
  lazily-evaluated when a property is first requested. The result is cached so the next time this property is
  accessed, you will get instant results."* Plus: *"This class is still pretty rudimentary and needs to be
  expanded as of beta."* <https://raw.githubusercontent.com/wiki/SerpentAI/SerpentAI/The-%27GameFrame%27-Class.md>
- Other measurements: `compare_ssim` via `skimage.measure.compare_ssim` on the 100×100 variant; `difference()` =
  gaussian-smoothed (`sigma=8`) grayscale subtraction; `top_color` = most frequent RGB triple of the
  **1/8-resolution** frame; `to_png_bytes(compress_level=3)`.
- `GameFrameBuffer(size=5)` — a 5-frame ring buffer; the agent default is
  `GameFrameBuffer(size=self.config.get("game_frame_buffer_size", 5))`.
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/game_frame_buffer.py>

### 2c. Detection / "object recognition" plugins

- **`SpriteIdentifier` — three modes behind one threshold API**,
  `identify(sprite, mode="SIGNATURE_COLORS", score_threshold=75)`:
  1. **`SIGNATURE_COLORS`** — score = `int(len(query_colors & sprite_colors) / len(sprite_colors) * 100)`;
     signatures are the **top-8 most frequent RGB triples** (`np.unique` + `np.argsort(counts)`), and
     **alpha==0 pixels are skipped** for 4-channel sprites.
  2. **`CONSTELLATION_OF_PIXELS`** — 8 sampled `(y,x) → exact RGB` points; **requires identical `image_shape`**;
     score = percentage matching by **exact tuple equality**. (CHANGELOG 0.1.1b1: *"sprite identifier needs to
     reject sprites that don't have the same shape as query sprite when using constellation of pixels"*.)
  3. **`SSIM`** — `int(skimage.measure.compare_ssim(q, s, multichannel=True) * 100)`.
  Below threshold it returns **`"UNKNOWN"`** — the framework's built-in "confirm before act".
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/sprite_identifier.py>
- **`Sprite`** carries an explicit performance TODO on the exact-colour locator:
  ```python
  @classmethod
  def locate_color(cls, color, image):
      # TODO: Optimize for ms gain
      color_indices = np.where(np.all(image[:, :, :3] == color, axis=-1))
  ```
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/sprite.py>
- **`SpriteLocator.locate()`** — the interesting trick: it does **not** scan the whole frame. It finds **one**
  constellation colour globally, subtracts that pixel's stored offset to generate candidate anchors, then
  **verifies all 8 constellation points** at each candidate. Supports `screen_region` cropping and
  `use_global_location` offset correction. Effectively an **anchor-pixel + verification** search.
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/sprite_locator.py>
- **OCR plugin — `pytesseract` + `skimage`, no ML** (`serpent/ocr.py`):
  - `extract_ocr_candidates(image, gradient_size=3, closing_size=10, minimum_area=100, minimum_aspect_ratio=2)`:
    `skimage.filters.rank.gradient` → **Otsu** → `morphology.closing(rectangle(1, closing_size))` →
    `measure.label` + `regionprops`; keeps regions with `area > 100`, `aspect_ratio >= 2`, more white than
    black pixels, `height >= 8`.
  - `perform_ocr(image, scale=10, order=5, horizontal_closing=10, vertical_closing=5)`: **upscale ×10**
    (`resize(..., mode="edge", order=5)`), Otsu, invert if mostly black, close horizontally then vertically,
    then `pytesseract.image_to_string`.
  - `locate_string(query_string, image, fuzziness=0, ...)` — exact match first, else **minimum Levenshtein
    (`editdistance.eval`) ≤ `fuzziness`**. `NativeWin32InputController.click_string` defaults **`fuzziness=2`**.
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/ocr.py>
- Also in-tree: `luminoth_object_recognizer.py` (Faster R-CNN), `cnn_xception_context_classifier.py`,
  `cnn_inception_v3_context_classifier.py`, `ppo_agent.py`, `rainbow_dqn_agent.py`, `recorder_agent.py`.
  <https://api.github.com/repos/SerpentAI/SerpentAI/git/trees/dev?recursive=1>

### 2d. Act loop

- **`NativeWin32InputController`** — same ctypes-`SendInput` approach as pygta5's `directkeys.py`, but complete:
  - Scancode mode `0x0008`; **extended keys use a `+1024` sentinel** in the mapping table, then
    `flags = 0x0008 | 0x0001` (`KEYEVENTF_EXTENDEDKEY`); release adds `| 0x0002`.
  - Mouse: absolute normalisation `windows_x = (x * 65535) // display_width`, flags `0x0001 | 0x8000`
    (`MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE`).
  - **Interpolated mouse motion** — `_interpolate_mouse_movement(..., steps=20)` via
    `scipy.interpolate.interp1d` + `np.linspace`, `time.sleep(duration / len(coordinates))` per step.
    Defaults: `duration=0.25` for `move`, `0.05` for `tap_key`/`click`.
  - Scroll: `clicks * 120`, flag `0x0800` (`MOUSEEVENTF_WHEEL`).
  - **Focus gating**: every action checks
    `("force" in kwargs and kwargs["force"] is True) or self.game_is_focused`.
  - **`click_sprite` / `click_string` return `False` when the target is not found** — agents branch on that
    instead of clicking blindly.
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/input_controllers/native_win32_input_controller.py>
- Input backends are pluggable: `CLIENT` (default, WAMP service), `PYAUTOGUI`, `NATIVE_WIN32`
  (`default_input_controller_backend = InputControllers.CLIENT`; CHANGELOG 0.1.6b1 *"InputController now pivots
  on a backend to allow extension"*).
- Two recorded performance fixes are useful precedent:
  - 0.1.6b1: *"stopped initializing a VisualDebugger instance on every new GameFrameBuffer. **Huge performance
    gain in frame consumption rate**."*
  - 2018.1.1: *"use interpolation order 0 in frame transformation pipeline resize"*.
  <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/CHANGELOG.md>

### 2e. Documented limitations

- **30 FPS default cap; 10 FPS when recording.**
- **Requires the game window focused**, else the loop idles at 1 s intervals.
- **Redis + Crossbar (WAMP)** multi-process plumbing; `serpent play` starts crossbar, an input-controller
  component, and a frame-grabber subprocess. Heavy for a single-game bot.
- **Windows/Linux only; macOS dropped** (CUDA).
- A consumer must wait for ≥150 buffered frames before the first frame is returned.
- Wiki still lists *"Using OCR (planned)"* and *"The Streaming API and Analytics Client (planned)"*.
  <https://raw.githubusercontent.com/wiki/SerpentAI/SerpentAI/Home.md>
- **Archived** (API `archived: true`), last push 2022-11-07; CHANGELOG frozen since 2018.1.2.

---

## 3. OSRS / RuneScape colour bots

### 3a. `ivan-guerra/colorbot` (Rust)

**Repo**: <https://github.com/ivan-guerra/colorbot> — **1★**, 0 forks, **Unlicense**, created 2024-12-08,
`pushed_at` **2026-08-04**, `archived: false`, Rust. Metadata: <https://api.github.com/repos/ivan-guerra/colorbot>

- **Capture**: the `scrap` crate — `Display::primary()` + `Capturer::new(...)`, **BGRA**, **full primary
  display, no ROI, no downscale**. It loops until `capturer.frame()` returns `Ok`
  (`loop { if let Ok(frame) = capturer.frame() { ...; break } }`) — honest handling of the frame-not-ready case.
  <https://raw.githubusercontent.com/ivan-guerra/colorbot/master/src/vision.rs>
- **Colour detection**: per-channel absolute difference with **`const TOLERANCE: u8 = 3`** (|ΔR|,|ΔG|,|ΔB| ≤
  tolerance). **Not HSV** — flat RGB with a tight tolerance, intended for **RuneLite Object Markers / NPC
  Indicators outlines** (README: *"This is best used with the outline function of the RuneLite Object Markers
  or NPC Indicators plugins"*).
- **Click-point selection (reusable)**: matched pixels → **convex hull (Graham scan)** → **ray-casting
  point-in-polygon** → sample up to **`MAX_ATTEMPTS = 1000`** candidates in the bbox and accept the first whose
  **minimum distance to any hull edge ≥ `MIN_EDGE_DISTANCE = 10.0`**; otherwise keep the best-so-far. Avoids
  clicking the anti-aliased outline.
- **Template matching**: capture → `to_luma8()` → `imageproc::template_matching::match_template_parallel` with
  **`MatchTemplateMethod::SumOfSquaredErrors`** (SSD, **not** normalised cross-correlation) →
  `find_extremes(...).min_value_location`. **Threshold is adaptive to template area**, not a fixed score:
  ```rust
  const MAX_PIXEL_VARIANCE: f32 = 15.0;
  let dynamic_threshold = (temp_width * temp_height) as f32 * MAX_PIXEL_VARIANCE.powi(2);
  if confidence_score <= dynamic_threshold { /* accept */ } else { /* bail */ }
  ```
  On success it returns a **random point inside the matched rect** so it does not click the same pixel twice.
- **Humanization**: `windmouse` module (WindMouse).
- **Platform**: **Linux/X11 only**, requires `xdotool`.
- **Scripting**: JSON events `keypress` / `color` / `image`; common props `type`, `id`, `count` (default 1),
  **`delay` = "Minimum delay in milliseconds after event execution"**.
- **Measured numbers: none published** → **UNVERIFIED**.
- README: <https://raw.githubusercontent.com/ivan-guerra/colorbot/master/README.md>

### 3b. `StaticSweep/ChromaScape` (Java; the most active of the set)

**Repo**: <https://github.com/StaticSweep/ChromaScape> — **56★**, 16 forks, **GPL-3.0**, created 2025-05-25,
`pushed_at` **2026-07-12**, `archived: false`, Java. Topics: `colour-detection`, `opencv`, `template-matching`,
`spring-boot`. Metadata: <https://api.github.com/repos/StaticSweep/ChromaScape>

**CV primitives**
- **HSV range thresholds**: *"The detection logic is optimized to handle slight variations in colour due to
  lighting or graphical effects by allowing for a lower and upper range of HSV colours."*
- **Contour topology**: `ColourContours.getChromaObjsInColour(gameView, COLOUR)` returns "ChromaObj" structures —
  colour mask → contours → objects.
- **Template matching with a low-is-better threshold**: `PointSelector.getRandomPointInImage(imagePath,
  gameView, threshold)` — *"A threshold of `0.05` is often preferred, with the maximum being `0.15`. A lower
  threshold means that the match needs to be more accurate."* (SQDIFF-family semantics, **not** a
  `TM_CCOEFF_NORMED` 0.8-style score.)
- **OCR = template matching of glyphs, explicitly not ML**: *"ChromaScape utilises template matching for
  accurate and fast OCR. This solution - as opposed to machine learning - provides for ocr at runtime. This
  was inspired by SRL and OSBC."* It **downloads fonts/UI elements from OSBC and SRL-dev** to build glyph
  templates (not vendored in-repo).
- **Zone system**: `controller().zones().getGameView()` is a `BufferedImage` (irregular shape); other zones
  (inventory slots, chatbox, control panel, minimap orbs) are **`Rectangle`s**, captured via
  `ScreenManager.captureZone(Rectangle)`.
  <https://raw.githubusercontent.com/StaticSweep/ChromaScape/main/README.md> ·
  <https://github-wiki-see.page/m/StaticSweep/ChromaScape/wiki/Making-your-first-script>

**Robustness tricks (official wiki; exact identifiers)**
- **Confirm-before-act on visual feedback**:
  `MovingObject.clickMovingObjectByColourObjUntilRedClick(COLOUR, this)` — *"Clicking continuously until the
  red cross (x) animation is detected."* On failure the script logs and **stops**: *"If the bot continued
  rather than stopping, it would likely click hundreds of times without being able to progress."*
- **State tracking by OCR of the XP counter, with a hard timeout instead of fixed sleeps**:
  `int previousXp = Minimap.getXp(this);` → on **`-1`** (read failure) the script **stops and notifies**; then
  `waitUntilXpChange(previousXp)` polls `waitMillis(300)` until `TIMEOUT_XP_CHANGE`. Followed by
  `waitRandomMillis(650, 800)`.
- **Occlusion handling**: *"For stacked/banked items (that have numbers over it) you need to crop out the
  **top 10 pixels**."* — a concrete template-hygiene rule.
- **Click-distribution "tightness"**: `PointSelector.getRandomPointByColourObj(gameView, MARK_COLOUR, 15, 15.0)`
  — tolerance **15**, tightness **15.0**; *"15.0 or more works best for ground items"*. Squeezes sampled clicks
  toward the tile centre so the click lands on the item, not the tile edge.
- **Double-check confirmation against render lag**: `recoverToResetTile()` begins with
  `waitRandomMillis(600, 800)` — *"Double check we are actually lost to protect against lag or rendering
  delays"* — then walks back with **`allowedAttempts = 5`** and `waitRandomMillis(4000, 6000)` to let the walk
  settle.
- **Confirmed-absence gate**: `isObstacleVisible()` uses
  `!ColourContours.getChromaObjsInColour(gameView, OBSTACLE_COLOUR).isEmpty()`;
  `waitForObstacleToAppear()` polls at 300 ms with `TIMEOUT_OBSTACLE_APPEAR`.
- **Humanization**: `waitRandomMillis(80, 100)` between keydown/keyup; **1% chance of a 2–5 minute break**
  (`random.nextInt(100) < 1` → `waitRandomMillis(120000, 300000)`); adapted WindMouse; and
  `ClickDistribution.generateRandomPoint(boundingBox)` for inventory slots.
- **Mouse parking / non-hijacking input**: **Brandon-T's RemoteInput** provides *"a virtual second mouse
  dedicated to the client window. Unlike traditional input methods, this approach never hijacks your physical
  mouse... also allows the user to completely minimise RuneLite while the bot is running."*
  <https://github.com/Brandon-T/RemoteInput>
- **Colour-picker workflow** (how HSV bounds are chosen): screenshot the client → in the web UI push the
  **`min` HSV sliders up until the colour almost disappears** and the **`max` sliders down until the colour is
  isolated** → name and save. <https://github-wiki-see.page/m/StaticSweep/ChromaScape/wiki/Colour-picker>
- **Measured numbers: none published for FPS/ms** → **UNVERIFIED**.
- Sources: <https://github-wiki-see.page/m/StaticSweep/ChromaScape/wiki/Intermediate-Scripting:-From-Planning-to-Execution> ·
  <https://raw.githubusercontent.com/wiki/StaticSweep/ChromaScape/Home.md>

### 3c. `kelltom/OS-Bot-COLOR` (OSBC) — archived, holds the only OSRS latency figure

**Repo**: <https://github.com/kelltom/OS-Bot-COLOR> — README opens with **"⚠️ This project is archived ⚠️"**.
Stars / `pushed_at` **UNVERIFIED** (GitHub API rate limit → 403).

- *"Unlike popular automation frameworks that modify/inject code into a game's client, OSBC is completely
  hands-off; it uses a combination of color detection, image recognition, and optical character recognition to
  navigate the game."*
- **Template matching with transparency**: *"We've modified OpenCV's template matching algorithm to be more
  efficient and reliable with UI elements and sprites - **even supporting images with transparency**."*
- **The one measured latency number in the OSRS set**: *"**OSBC can locate text on screen in as little as 2
  milliseconds.** That's 0.002 seconds."* Self-reported, **no hardware stated** → vendor claim.
- Also: Bezier-curve human-like mouse movement; randomized click distribution; **Python 3.10 only** (documented
  as not compatible with other major versions).
- **It is the upstream template/OCR source for ChromaScape.**
- Source: <https://raw.githubusercontent.com/kelltom/OS-Bot-COLOR/main/README.md>

### 3d. `Villavu/SRL-Development` (SRL, Simba) — richest documented CV primitive set

**Repo**: <https://github.com/Villavu/SRL-Development> — *"SRL is a library that provides an API for writing bots
in Simba for the game Old School RuneScape."* Docs: <https://villavu.github.io/SRL-Development/>
Stars / `pushed_at` **UNVERIFIED** (rate limit).

| Concern | SRL identifiers | URL |
|---|---|---|
| Motion / animation detection | `SRL.GetPixelShift`, `SRL.GetPixelShiftTPA`, `SRL.IsAnimating` | <https://villavu.github.io/SRL-Development/pixelshift.html> |
| Humanised randomness | `SRL.GaussRand`, `SRL.TruncatedGauss`, `SRL.SkewedRand`, `SRL.NormalRange`, `SRL.RandomPoint`, `SRL.RandomPointEx`, `SRL.rowp` | <https://villavu.github.io/SRL-Development/random.html> |
| **Mouse parking / anti-detection** | `Mouse.WindMouse`, **`Mouse.Idle`**, **`Mouse.Miss`** (deliberate mis-click), `Mouse.Teleport`, `Mouse.Setup(EMouseDistribution)` | <https://villavu.github.io/SRL-Development/mouse.html> |
| Break scheduling | `Antiban.AddSleep`, `Antiban.AddBreak`, `Antiban.AddTask`, `Antiban.DoAntiban`, "Built in Antiban Tasks" | <https://villavu.github.io/SRL-Development/antiban.html> |
| **Confirm-before-act** | `Mainscreen.DidRedClick`, `Mainscreen.DidYellowClick` | <https://villavu.github.io/SRL-Development/mainscreen.html> |
| HUD detection | `Mainscreen.FindHPBars`, `Mainscreen.FindHitsplats` | <https://villavu.github.io/SRL-Development/mainscreen.html> |
| HUD OCR | `XPBar.Read`, `Chat.GetLineBoxes`, `Chat.GetDisplayName`, `Chat.GetChat`, `Chat.GetQuery`, `Inventory.CountItem` | <https://villavu.github.io/SRL-Development/xpbar.html> · <https://villavu.github.io/SRL-Development/chat.html> |
| **Multi-scale / rotation template search** | `FindDTM`, `FindDTMs`, **`FindDTMRotated`, `FindDTMRotatedSE`, `FindDTMRotatedAlternating`**, `FindBitmapIn`, **`FindBitmapToleranceIn`** (DTM = Deformable Template Model) | <https://villavu.github.io/SRL-Development/wrappers.html> |
| Colour-tolerance modes | `CTS0`, `CTS1`, `CTS2` (Color Tolerance Speed), `SRL.FindColors`, `FindColorsTolerance`, `CountColorTolerance` | <https://villavu.github.io/SRL-Development/color.html> |
| Contour → geometry | `TPointArray.ConvexHull`, `MinAreaRect`, `MinAreaCircle`, `Cluster`, `Edges`, `Bounds`, `SplitRows`, `FilterBox`, `FilterDuplicates`, `Density`; `TRectangle.Partition`, `NearestEdge`; `TBox.Intersect/Invert` | <https://villavu.github.io/SRL-Development/tpointarrays.html> |
| **Coordinate transforms (ROI / zoom)** | `Minimap.ArrToMs/VecToMs/PointToMs/GetTileMS`, `Minimap.StaticToMsRect`, `Minimap.GetZoomRectangle`, `Mainscreen.PointToMM`, `Options.SetZoomLevel` | <https://villavu.github.io/SRL-Development/mm2ms.html> |
| **Multi-scale map matching** | `Walker.CleanMinimap`, `Walker.GetCleanMinimap`, **`Walker.ScaleMinimap`, `Walker.ScaledSearch`, `Walker.FullSearch`**, `TRSWalkerMap.FindMap`, `Walker.GetMyPos` | <https://villavu.github.io/SRL-Development/walker.html> |
| **Mouse parked ahead along the path** | **`Walker.DoMouseAhead`**, `Walker.AdaptiveWalkCheck`, `Walker.WaitMoving` | <https://villavu.github.io/SRL-Development/walker.html> |
| HUD state gates before acting | `Bank.IsOpen`, `Inventory.IsOpen`, `CraftScreen.IsOpen`, `Prayer.IsOpen` … | <https://villavu.github.io/SRL-Development/bank.html> |

SRL README advertises **resizable-mode support** and **minimap→mainscreen projection** as headline features:
<https://raw.githubusercontent.com/Villavu/SRL-Development/master/README.md>

### 3e. `cemenenkoff/runedark-public` (RuneDark)

**Repo**: <https://github.com/cemenenkoff/runedark-public> — Windows, Python **3.10.9**, black-formatted.
Stars / `pushed_at` **UNVERIFIED**.

- *"Unlike traditional injection or reflection frameworks, RuneDark takes a hands-off approach, leveraging
  computer vision and optical character recognition for precise and efficient automation."*
- Feature taxonomy worth quoting: **Object Detection** ("Detects and converts in-game objects into data
  structures"), **Image Recognition** ("Identifies images within images using computer vision"),
  **"Color-on-Color OCR: Reads text on varying font and background colors reliably"**, and **Humanization**
  ("Adds randomness to mouse movements, wait times, and keystrokes").
- <https://raw.githubusercontent.com/cemenenkoff/runedark-public/main/README.md>

---

## 4. Pokémon bots (vision-based)

### 4a. `PWhiddy/PokemonRedExperiments` — verified vision-based (PyBoy + stable-baselines3)

**Repo**: <https://github.com/PWhiddy/PokemonRedExperiments> — **7,907★**, 788 forks, **MIT**, created
2019-12-25, `pushed_at` **2026-09-11**, `archived: false`.
**`peterfredholm/PokemonRedExperiments` returns 404 — does not exist.**

- **Perception = PyBoy framebuffer pixels** for the policy observation:
  `game_pixels_render = self.pyboy.screen.ndarray[:,:,0:1]  # (144, 160, 3)`
  → `downscale_local_mean(game_pixels_render, (2,2,1))` → **(72, 80, 1)**;
  `self.output_shape = (72, 80, self.frame_stacks)` with **`self.frame_stacks = 3`** → screens obs **(72,80,3)**.
- **Action repeat / frame skip**: `'action_freq': 24`; `run_action_on_emulator` uses `press_step = 8` →
  `pyboy.tick(8, render_screen)` → release → `pyboy.tick(act_freq - press_step - 1, render_screen)` →
  `pyboy.tick(1, True)`. So **24 emulated frames per agent step, button held for 8 of them**.
- **Frame stacking** via `np.roll(self.recent_screens, 1, axis=2)` (roll-then-overwrite).
- Full obs is a `spaces.Dict`: `screens` (72,80,3) uint8, `health` (0,1), `level` (8,) Fourier-encoded
  (`enc_freqs = 8`), `badges` MultiBinary(8), `events` MultiBinary((0xD87E-0xD747)*8), `map` (48,48,1)
  (`coords_pad = 12`), `recent_actions` MultiDiscrete([7]*3). Action space `Discrete(7)`
  (DOWN/LEFT/RIGHT/UP/A/B/START).
- **Hybrid caveat**: pixels feed the *observation*, but **RAM is read for dense reward shaping** (event flags
  0xD747–0xD87E, badges 0xD356, party levels, HP 0xD16C / max-HP 0xD18D). It is *"vision-observation,
  memory-supervised"*, **not** a pure screen-only agent.
- V1 (`baselines/red_gym_env.py`): `downsample_factor = 2`, `output_shape = (36, 40, 3)`, `frame_stacks = 3`,
  and **KNN novelty exploration** (`hnswlib.Index(space='l2', dim=4320)`,
  `init_index(max_elements=20000, ef_construction=100, M=16)`; novelty when `distances[0][0] > similar_frame_dist`).
- README claims for V2: *"Trains faster and with less memory"*, **"Reaches Cerulean"**, *"Replaces the frame KNN
  with a coordinate based exploration reward"*. **There is NO "completed the game" claim** → treat any such
  claim as **UNVERIFIED**. Follow-up paper **arXiv 2502.19920** (linked from the README; paper not fetched).
- Required ROM: `PokemonRed.gb`, **sha1 `ea9bcae617fdf159b045185467ae58b2e4a48b9a`**, 1 MB.
- Training scale (`v2/baseline_fast_v2.py`): `ep_length = 2048 * 80` = **163,840**; `num_cpu = 64`
  `SubprocVecEnv`; PPO `n_steps = 2560`, `batch_size=512`, `n_epochs=1`, `gamma=0.997`, `ent_coef=0.01`;
  `total_timesteps = ep_length * 64 * 10000` ≈ **1.05e11**; `reward_scale=0.5`, `explore_weight=0.25`.
- URLs: <https://raw.githubusercontent.com/PWhiddy/PokemonRedExperiments/master/README.md> ·
  <https://raw.githubusercontent.com/PWhiddy/PokemonRedExperiments/master/v2/red_gym_env_v2.py> ·
  <https://raw.githubusercontent.com/PWhiddy/PokemonRedExperiments/master/v2/baseline_fast_v2.py> ·
  <https://raw.githubusercontent.com/PWhiddy/PokemonRedExperiments/master/baselines/red_gym_env.py>

### 4b. `Baekalfen/PyBoy` — emulator with published throughput numbers

**Repo**: <https://github.com/Baekalfen/PyBoy> — **5,197★**, 538 forks, created 2015-05-29,
`pushed_at` **2026-09-14**, `archived: false`, license **NOASSERTION** (not a clean SPDX id).

- **Screen access**: README documents `pil_image = pyboy.screen.image` → PIL image. The **numpy framebuffer**
  path `pyboy.screen.ndarray[:,:,0:1] # (144,160,3)` is **not in the README** but is used in production
  (PokemonRedExperiments V2). Legacy API `pyboy.botsupport_manager().screen().screen_ndarray()` in V1.
- `tick()` semantics: `pyboy.tick() # Process at least one frame to let the game register the input`;
  `pyboy.set_emulation_speed(0) # No speed limit`; memory via `pyboy.memory[0xC345]`.
- **Published performance table (README, "higher is better")**:
  - Full rendering: **×124 realtime**
  - **Frame-skip 15 (`pyboy.tick(15)`): ×344 realtime**
  - **No rendering (`pyboy.tick(target, False)`): ×395 realtime**
  - README conversions: the Game Boy ran at **60 fps**, so ×100 realtime = **6,000 fps**;
    *"simulating 395 hours of gameplay can be done in 1 hour"*; *"On an 8-core machine, you could potentially
    do 3160 hours of gameplay in 1 hour."*
- <https://raw.githubusercontent.com/Baekalfen/PyBoy/master/README.md>
- The `screen` class API docs (<https://docs.pyboy.dk/#pyboy.PyBoy.tick>, <https://baekalfen.github.io/PyBoy/index.html>)
  were **not fetched → UNVERIFIED**.

### 4c. Pokémon CV-bot gaps

- `DBJoran/Shinyhunter` — **4★**, `pushed_at` 2023-10-03, *"bot that will search/hunt for shiny Pokémon in
  Pokémon Leaf Green and Fire Red... uses the **VisualBoyAdvance-1.0.8-beta3** emulator"*.
  <https://github.com/DBJoran/Shinyhunter>
  **Detection method UNVERIFIED — source not read.**
- **No OCR-or-template-matching-on-Pokémon-HUD bot was found.** PyBoy projects read HP/levels from **RAM**
  (`read_hp`, `read_hp_fraction` at 0xD16C/0xD18D). Treat "OCR of Pokémon HUD" as **NOT FOUND**.
- `AutoPokemon` / `PokeBot` / `pokemon-bot` / `PokeMMO bot`: **NOT FOUND → UNVERIFIED**.

---

## 5. Diablo / WoW / ARPG pixel bots

### 5a. `johannes-do/botty` — D2R pixel bot, archived, documented match scores

**Repo**: <https://github.com/johannes-do/botty> — **572★**, **MIT**, `pushed_at` **2022-07-10**,
**`archived: true`**, Python, *"D2R Pixel Bot"*. Metadata: <https://api.github.com/repos/johannes-do/botty>

- **Resolution**: *"Botty currently works with a **720p D2R window** (will be adjusted automatically on auto
  settings)."*
- **Detection = scored template matching with a published threshold**: the Graphic Debugger (default key
  **F10**) *"will print out the scores for each item that would be picked up. **Scores should be well above 0.9**
  for these items."* It also renders *"templates with blue circles... and scores"* → masked/scored matching.
- **Robustness / recovery constants (README)**: `casting_frames` — *"Determines how much delay there is after
  each teleport"* (frame-count timing tuned to FCR breakpoints = **action cooldown**); `max_consecutive_fails`;
  `max_game_length_s`; `restart_d2r_when_stuck=1`; `chicken` / `merc_chicken` HP-percent bail-outs;
  `info_screenshots=1` (screenshot on stuck/chicken/timeout/inventory-full); `pickit_screenshots`;
  `runs_per_stash`; `runs_per_repair`.
- Hotkeys: **f9** adjust settings, **f11** start, **f12** force stop. Requires the **English** client.
- **Capture API (GDI/DXGI/MSS): UNVERIFIED** — not stated in README; source not read. **No FPS/ms numbers published.**
- Predecessor `aeon0/botty` — **metadata UNVERIFIED (403)**; release links point to
  <https://github.com/aeon0/botty/releases>; snapshot
  <https://archive.org/details/github.com-aeon0-botty_-_2021-11-07_15-28-24>.
- README: <https://raw.githubusercontent.com/johannes-do/botty/master/README.md>

### 5b. `Xian55/WowClassicGrindBot` — **assisted** pixel readout, not general CV

**Repo**: <https://github.com/Xian55/WowClassicGrindBot> — README fetched HTTP 200 (repo + `main` branch exist);
**stars / `pushed_at` / archived UNVERIFIED (403 rate limit)**.

- **Critical nuance**: *"The bot reads the game state using small blocks of colour shown at the top of the
  screen by an **Addon**"* — addon **Happy-Pixels**. Backend: *"written in C#. **Screen capture, mouse and
  keyboard clicking. No memory tampering and no DLL injection.**"* → a screen-**pixel** bot whose pixels are
  *drawn by an in-game Lua addon* as colour-coded data blocks. **Classify separately from true CV bots.**
- One genuine CV component: **`NpcNameFinder`** — finds *"friendly, enemy, corpse - names above NPCs head"*.
  README advises replacing the default `FRIZQT__.ttf` with a bolder font (e.g. Roboto-Medium) for *"big
  improvement to the NpcNameFinder component"* — a real-world confirmation that **font/anti-aliasing changes
  directly affect detection quality**.
- Resolution: **4:3 aspect-ratio based**, tested at **1024×768 / 1920×1080 / 3840×2160**, fullscreen or windowed.
- **Forced environment settings (README)**: Contrast `50`, Brightness `50`, Gamma `1.0`, Render Scale `100%`,
  `/console ffxGlow 0`, Nvidia "Image Sharpening → **Sharpening Off, Scaling disabled**".
  KeyAction defaults: **`PressDuration` = 50 ms**, **`DelayAfterCast` = 1450 ms**, `Cooldown` = 0,
  `Cost` = 18, `Charge` = 1, `WaitForGCD` = true, `SkipValidation` = false, `ResetOnNewTarget` = false;
  `"PathReduceSteps": true` = "uses every other coordinate" (waypoint thinning).
- Pathing deps: PPather <https://github.com/namreeb/PPather> · AmeisenNavigation
  <https://github.com/Xian55/AmeisenNavigation> · architecture write-up <http://www.codesin.net/post/wowbot/>
- **Capture API UNVERIFIED** (`Core/ScreenCapture.cs` guess → 404). **No FPS/ms numbers.**

### 5c. Other D2/D2R and CV bots (metadata-verified; internals largely UNVERIFIED)

| Repo | Stars | pushed_at | Archived | Lang | Note |
|---|---|---|---|---|---|
| <https://github.com/bouletmarc/D2R-BMBot> | 104 | 2024-06-28 | no | C# | "Diablo II : Resurrected - Bot"; pixel-vs-memory UNVERIFIED |
| <https://github.com/dulingzhi/koolo> | 10 | 2023-11-27 | no | Go | MIT; "D2R bot written in Go" |
| <https://github.com/vdamov/D2R-AI-Item-Tracker> | 7 | 2025-09-03 | no | Python | MIT; **offline screenshot OCR via Vision LLMs**, not a realtime bot |
| <https://github.com/narsdk/d2cv> | 2 | 2022-05-31 | no | Python | "**Computer Vision bot for diablo 2**"; internals UNVERIFIED |
| <https://github.com/Quadrat1c/D2RPixPy> | 1 | 2022-03-09 | no | Python | "D2R Pixel Bot in Python" |
| <https://github.com/goblindevelopment/D2R-Bots> | 1 | 2024-01-25 | no | — | explicitly **memory-based** → good *exclusion* example |
| <https://github.com/Lime365/wow-fishbot> | 11 | 2024-09-16 | no | Python | "Fishing bot for world of warcraft using python and **open-cv**"; internals UNVERIFIED |

### 5d. Generic CV game-bot frameworks / other titles

| Repo | Stars | pushed_at | Archived | License | Note |
|---|---|---|---|---|---|
| <https://github.com/paulonteri/play-game-with-computer-vision> | 142 | 2022-12-01 | **yes** | Apache-2.0 | City Island 5 bot; best-documented generic example in this set |
| <https://github.com/steve1316/granblue-automation-pyautogui> | 132 | 2023-10-14 | no | none | "**OpenCV, PyAutoGUI and EasyOCR**" |
| <https://github.com/Sid-1996/PathofExile-Sid-GameTools_HealthMonitor> | 11 | 2026-08-30 | no | AGPL-3.0 | **Path of Exile** health/mana monitoring + click automation, OpenCV + PySide6 |
| <https://github.com/Tanmoy-Mondal-07/Python-Game-Bot> | 12 | 2025-05-26 | no | MIT | OpenCV + PyAutoGUI |
| <https://github.com/KarahanKARA/eatventure-bot> | 9 | 2026-02-18 | no | Apache-2.0 | OpenCV + scrcpy (Android mirroring) |
| <https://github.com/Long173/onmyoji-auto-releases> | 9 | 2026-09-15 | no | — | Python/PyQt5/OpenCV |
| <https://github.com/yehuoshun/czn-auto> | 7 | 2026-06-29 | **yes** | — | Python + OpenCV + Win32 API |
| <https://github.com/Dhi13man/edge-surf-pid> | 3 | 2026-09-11 | no | MIT | OpenCV + PyInput |
| <https://github.com/MetinOpenBot/OpenBot> | 75 | 2022-04-01 | no | none | Metin2 lineage; **CV-based UNVERIFIED** |

- `OpenBot` disambiguation: the high-star hits are **not game bots** — `ob-f/OpenBot` (3,497★) is the
  smartphone **robotics** platform <https://github.com/ob-f/OpenBot>; `CopilotKit/OpenBot` (4,979★) is LLM
  agent tooling.
- **"BotForge", "Adept", "lunaris", "pixelbot" as generic frameworks: NOT FOUND → UNVERIFIED.**
- **`WoW-5.4.8-OpenBot`: NOT FOUND → do not cite.** **OSBot: UNVERIFIED.**

### 5e. Cross-cutting gaps for this cluster

1. **Capture API (GDI `BitBlt` / DXGI Desktop Duplication / `mss` / X11) is UNVERIFIED for every repo in this
   cluster.** No README stated it and no fetched source confirmed it. Largest hole vs. the requested field list.
2. **No FPS / ms-per-frame / ms-per-locate numbers exist in any Diablo/WoW repo found.**
3. `Xian55/WowClassicGrindBot` and `aeon0/botty` star counts are **UNVERIFIED (403, not 404 — they likely exist)**.

---

## 6. RL agents with screen input — frame skip / frame stack / action repeat

### 6a. The canonical recipe — OpenAI Baselines

**File**: <https://raw.githubusercontent.com/openai/baselines/master/baselines/common/atari_wrappers.py>

- **`MaxAndSkipEnv`** — the canonical `k = 4` action repeat:
  ```python
  class MaxAndSkipEnv(gym.Wrapper):
      def __init__(self, env, skip=4):
          """Return only every `skip`-th frame"""
          ...
          # most recent raw observations (for max pooling across time steps)
          self._obs_buffer = np.zeros((2,)+env.observation_space.shape, dtype=np.uint8)
          self._skip       = skip
  ```
  `step()` docstring: **"Repeat action, sum reward, and max over last observations."** It steps the env `skip`
  times with the same action, sums reward, keeps frames `skip-2` and `skip-1`, returns
  `self._obs_buffer.max(axis=0)`. Comment: *"Note that the observation on the done=True frame doesn't matter"*.
  → **max-pooling over the last two frames exists to suppress Atari sprite flicker.**
- **`WarpFrame(env, width=84, height=84, grayscale=True, ...)`** — docstring *"Warp frames to 84x84 as done in
  the Nature paper and later work."*; `cv2.cvtColor(frame, cv2.COLOR_RGB2GRAY)` →
  `cv2.resize(frame, (84,84), interpolation=cv2.INTER_AREA)` → `np.expand_dims(frame, -1)`.
- **`FrameStack(env, k)`** — docstring *"Stack k last frames. Returns lazy array, which is much more memory
  efficient."*; `deque([], maxlen=k)`; `observation_space = Box(..., shape=(shp[:-1] + (shp[-1]*k,)))`.
- **`LazyFrames`** — *"This object ensures that common frames between the observations are only stored once. It
  exists purely to optimize memory usage which can be huge for **DQN's 1M frames replay buffers**."*
- **`NoopResetEnv(noop_max=30)`** — *"Sample initial states by taking random number of no-ops on reset. No-op is
  assumed to be action 0."*; `noops = self.unwrapped.np_random.randint(1, self.noop_max + 1)`.
- **`EpisodicLifeEnv`** — *"Make end-of-life == end-of-episode, but only reset on true game over. Done by
  DeepMind for the DQN and co. since it helps value estimation."*
- **`ClipRewardEnv`** — `return np.sign(reward)`.
- **`ScaledFloatFrame`** — *"careful! This undoes the memory optimization, use with smaller replay buffers only."*
- **Factories**:
  - `make_atari(env_id, max_episode_steps=None)`: `assert 'NoFrameskip' in env.spec.id` →
    `NoopResetEnv(env, noop_max=30)` → `MaxAndSkipEnv(env, skip=4)`.
  - `wrap_deepmind(env, episode_life=True, clip_rewards=True, frame_stack=False, scale=False)` →
    `EpisodicLifeEnv` → `FireResetEnv` → `WarpFrame` → `ClipRewardEnv` → **`if frame_stack:
    env = FrameStack(env, 4)`**. **Canonical stack = 4**, but note stacking is **OFF by default in this
    factory** — a real reproducibility footgun.

### 6a-bis. Retro wrappers live in Baselines, not in `retro`

**File**: <https://raw.githubusercontent.com/openai/baselines/master/baselines/common/retro_wrappers.py>

- `class StochasticFrameSkip(gym.Wrapper): def __init__(self, env, n, stickprob)`.
- `def make_retro(*, game, state=None, max_episode_steps=4500, **kwargs): ... env = StochasticFrameSkip(env, n=4,
  stickprob=0.25)`.
- `def wrap_deepmind_retro(env, scale=True, frame_stack=4)` → `WarpFrame` → `ClipRewardEnv` →
  `if frame_stack > 1: env = FrameStack(env, frame_stack)` → `ScaledFloatFrame`. (**Here `frame_stack` defaults
  to 4**, unlike `wrap_deepmind`.)
- Rationale comments inline: `# First step after reset, use action` / `# First substep, delay with
  probability=stickprob` / `# Second substep, new action definitely kicks in`.
- Also defined there: `PartialFrameStack`, `Downsample`, `Rgb2gray`, `AppendTimeout`, `SonicDiscretizer`,
  `RewardScaler`, `AllowBacktracking`.
- **`StochasticFrameSkip` is NOT in `retro` or `stable-retro`** — Stable-Retro merely re-pastes a copy into a
  docs example (n=4, stickprob=0.25, TimeLimit 4500, `VecFrameStack(..., n_stack=4)`, PPO 100M steps):
  <https://raw.githubusercontent.com/Farama-Foundation/Stable-Retro/master/docs/index.md>

### 6b. stable-baselines3 `AtariWrapper` — current, best-documented defaults

**File**: <https://raw.githubusercontent.com/DLR-RM/stable-baselines3/master/stable_baselines3/common/atari_wrappers.py>
Docs: <https://stable-baselines3.readthedocs.io/en/master/common/atari_wrappers.html>

- **Exact signature / defaults**:
  ```python
  AtariWrapper(env, noop_max=30, frame_skip=4, screen_size=84,
               terminal_on_life_loss=True, clip_reward=True,
               action_repeat_probability=0.0)
  ```
- **Docstring bullets (verbatim)**: *"Noop reset: obtain initial state by taking random number of no-ops on
  reset."* / *"**Frame skipping: 4 by default**"* / *"Max-pooling: most recent two observations"* /
  *"Termination signal when a life is lost."* / *"**Resize to a square image: 84x84 by default**"* /
  *"Grayscale observation"* / *"Clip reward to {-1, 0, 1}"* / *"**Sticky actions: disabled by default**"*.
- **Explicit action-repeat equivalence in a code comment**:
  `# frame_skip=1 is the same as no frame-skip (action repeat)`.
- **Documented precondition (common footgun)**: *"Use this wrapper only with Atari v4 without frame skip:
  `env_id = "*NoFrameskip-v4"`."* → wrapping an already-skipped env multiplies the repeat.
- `MaxAndSkipEnv` docstring: *"Return only every ``skip``-th frame (frameskipping) and return the max between
  the two last frames."*; param doc: *"`skip`: Number of ``skip``-th frame. The same action will be taken
  ``skip`` times."*
- `WarpFrame` docstring: *"Convert to grayscale and warp frames to 84x84 (default) as done in the Nature paper
  and later work."*; observation space `(84, 84, 1)`.
- **Build order** (matters for reproduction): `StickyActionEnv` (only if `action_repeat_probability > 0`) →
  `NoopResetEnv` → `MaxAndSkipEnv` (only if `frame_skip > 1`) → `EpisodicLifeEnv` → `FireResetEnv` →
  `WarpFrame` → `ClipRewardEnv`.
- **Correction to common belief**: SB3's `AtariWrapper` has **no `frame_stack` and no `episodic_life`
  parameter** — frame stacking is a separate `VecFrameStack`/`FrameStack`, and life-loss termination is spelled
  `terminal_on_life_loss`.
- **No range validation for `frame_skip`** — the only guard is `if frame_skip > 1`, so `1` and `0` both mean
  "no skip". **Documented valid range: none → UNVERIFIED.**
- `StickyActionEnv` cites **arXiv:1709.06009**.
- Repo <https://github.com/DLR-RM/stable-baselines3> — **~14k★** (rounded badge); docs version string
  *"Stable Baselines3 2.9.2a0"*.

### 6c. `openai/retro` and Stable-Retro — **no frame skip or stack at all**

- **`openai/retro`**: <https://github.com/openai/retro> — **3,586★**, 535 forks, open issues 62, created
  2018-02-07, `pushed_at` **2024-02-22T13:04:14Z**, **`archived: true`**, **MIT**, default branch `master`.
  Metadata: <https://api.github.com/repos/openai/retro>
  - `RetroEnv.__init__` has **no `frame_skip`, no `stacked`, no `frameskip=`** parameter, and therefore no range
    check or error message. Signature:
    `(game, state=retro.State.DEFAULT, scenario=None, info=None, use_restricted_actions=retro.Actions.FILTERED,
    record=False, players=1, inttype=retro.data.Integrations.STABLE, obs_type=retro.Observations.IMAGE)`.
  - `metadata = {'render.modes': ['human','rgb_array'], 'video.frames_per_second': 60.0}`.
  - `step()` calls `self.em.step()` **exactly once** — no action repeat.
  - `get_screen(player=0)` = `self.em.get_screen()` cropped by `self.data.crop_info(player)` — **no downscale,
    no grayscale, no stacking**.
  - `observation_space = Box(low=0, high=255, shape=shape, dtype=np.uint8)` where `shape` is the **per-game
    crop**, so it is game-dependent. Docs only say *"The default observations are RGB images of the game"*:
    <https://retro.readthedocs.io/en/latest/python.html>. **"224×224 RGB by default" is UNVERIFIED.**
  - Frame skip enters via Baselines: `retro/examples/ppo.py` →
    `from baselines.common.retro_wrappers import make_retro, wrap_deepmind_retro`.
  - Docs status line: *"**Status:** Maintenance (expect bug fixes and minor updates)"* —
    <https://retro.readthedocs.io/en/latest/index.html>
  - <https://raw.githubusercontent.com/openai/retro/master/retro/retro_env.py>
- **`Farama-Foundation/Stable-Retro`** (the real maintained fork): <https://github.com/Farama-Foundation/Stable-Retro>
  — **1,078★**, 84 forks, created 2023-05-15, `pushed_at` **2026-09-15T14:01:40Z**, **`archived: false`**,
  **MIT**, default branch `main`, *"A fork of gym-retro with additional games, emulators and supported
  platforms"*, homepage <https://stable-retro.farama.org/>.
  - `class RetroEnv(gym.Env, EzPickle)`; same absence of frame-skip/stack params; `step()` steps once;
    `get_screen(self, player=0, apply_rotation=False)` with `_rotation_steps()` / `_apply_rotation()` for
    rotated cores. <https://raw.githubusercontent.com/Farama-Foundation/Stable-Retro/main/stable_retro/retro_env.py>
  - **`Farama-Foundation/Gym-Retro` → 404, does not exist.**

### 6d. DQN — the primary source, verbatim

**Mnih et al. 2013 (arXiv:1312.5602)** — full text <https://ar5iv.labs.arxiv.org/html/1312.5602> (abs
<https://arxiv.org/abs/1312.5602>):

> *"Following previous approaches to playing Atari games, we also use a simple frame-skipping technique [3].
> More precisely, **the agent sees and selects actions on every kth frame instead of every frame, and its last
> action is repeated on skipped frames. Since running the emulator forward for one step requires much less
> computation than having the agent select an action, this technique allows the agent to play roughly k times
> more games without significantly increasing the runtime. We use k=4 for all games except Space Invaders where
> we noticed that using k=4 makes the lasers invisible because of the period at which they blink. We used k=3 to
> make the lasers visible** and this change was the only difference in hyperparameter values between any of the
> games."*

> *"The raw frames are preprocessed by first converting their RGB representation to gray-scale and down-sampling
> it to a 110×84 image. The final input representation is obtained by cropping an 84×84 region of the image…
> For the experiments in this paper, the function φ from algorithm 1 **applies this preprocessing to the last 4
> frames of a history and stacks them** to produce the input to the Q-function."* / *"The input to the neural
> network consists is an **84×84×4 image** produced by φ."*

Also: input is *"**210×160 RGB video at 60Hz**"*; rewards clipped to ±1; *"**minibatches of size 32**…
**10 million frames** and… a replay memory of **one million most recent frames**."*

**DQN Nature 2015 (doi:10.1038/nature14236) — text NOT retrievable** (nature.com redirects to
`idp.nature.com` auth; the DeepMind PDF is rejected as `unsupported content type "application/pdf"`).
**The exact Nature-2015 frame-skip sentence is UNVERIFIED.** Best corroboration that Nature 2015 specifies
frame-skip 4 + max-pool of the last two frames + 84×84 comes from Dopamine's `AtariPreprocessing` docstring
(§6f).

### 6e. Why frame skip / stack exist — cited rationale

- **Machado et al. 2018, "Revisiting the ALE" (arXiv:1709.06009)** — full text
  <https://ar5iv.labs.arxiv.org/html/1709.06009>:
  > *"**1) Frame skipping restricts the agent's decision points by repeating a selected action for k consecutive
  > frames. Frame skipping results in a simpler reinforcement learning problem and speeds up execution; values
  > of k=4 and k=5 have been commonly used in the literature.** 2) Color averaging and frame pooling are two
  > image-based mechanisms to flatten two successive frames into a single one in order to reduce visual artifacts
  > resulting from limitations of the Atari 2600 hardware… Finally, 3) **frame stacking concatenates previous
  > frames with the most recent in order to construct a richer observation space for the agent. Frame stacking
  > also reduces the degree of partial observability in the ALE, making it possible for the agent to detect the
  > direction of motion in objects.**"*
  Also: *"**A frame (as a unit of time) corresponds to 1/60th of a second**"*; sticky actions
  *"if ς=0.25, there is 25% chance the environment will not execute the desired action right away"*;
  *"fairly standard to train agents for **200 million frames**… approximately **38 days of real-time
  gameplay**"*; and a measurement warning: *"we advocate taking the number of skipped frames into consideration
  when measuring training data."*
- **Baselines / SB3 one-liners** (URLs above): *"Return only every `skip`-th frame"*; *"Repeat action, sum
  reward, and max over last observations."*; `# frame_skip=1 is the same as no frame-skip (action repeat)`.
- **Dopamine** — *"# Strip out the TimeLimit wrapper from Gym, which caps us at 100k frames. We handle this time
  limit internally instead, which lets us cap at **108k frames (30 minutes)**."*
  <https://raw.githubusercontent.com/google/dopamine/master/dopamine/discrete_domains/atari_lib.py>
- **EnvPool** — `max_episode_steps` default 27000 *"which corresponds to **108000 frames** or roughly **30
  minutes of game-play** (Hessel et al. 2018, Table 3) **because of the 4 skipped frames**"*.
  <https://envpool.readthedocs.io/en/latest/env/atari.html>
- **ACME** — *"# Action repeat frames should not span episode boundaries."*
  <https://raw.githubusercontent.com/google-deepmind/acme/master/acme/wrappers/atari_wrapper.py>
- **Sample Factory** (the cleanest statement of the metrics trap): *"**FPS metrics will be multiplied by the
  frameskip value, i.e. 100000FPS with frameskip=4 actually corresponds to 100000/4=25000 samples per second
  observed by the policy. Frameskip=1 (default) means no frameskip, we process every frame.**"*
  <https://raw.githubusercontent.com/alex-petrenko/sample-factory/master/sample_factory/cfg/cfg.py>

### 6f. Cross-framework constant table

| Project | frame skip / action repeat | frame stack | observation | URL |
|---|---|---|---|---|
| openai/retro (**archived**) | **none** (1 frame per `step`) | **none** | RGB uint8, per-game crop; `video.frames_per_second: 60.0` | <https://raw.githubusercontent.com/openai/retro/master/retro/retro_env.py> |
| Stable-Retro | **none** in env; docs recipe `n=4, stickprob=0.25` | docs recipe `n_stack=4` | RGB uint8 + optional rotation fix | <https://raw.githubusercontent.com/Farama-Foundation/Stable-Retro/main/stable_retro/retro_env.py> · <https://raw.githubusercontent.com/Farama-Foundation/Stable-Retro/master/docs/index.md> |
| baselines (Atari) | `MaxAndSkipEnv(skip=4)` | `FrameStack(env, 4)` via `wrap_deepmind(frame_stack=…)` — **default False** | `WarpFrame` 84×84×1 gray | <https://raw.githubusercontent.com/openai/baselines/master/baselines/common/atari_wrappers.py> |
| baselines (retro) | `StochasticFrameSkip(n=4, stickprob=0.25)`, `make_retro(max_episode_steps=4500)` | `wrap_deepmind_retro(frame_stack=4)` | `WarpFrame` 84×84 + `ScaledFloatFrame` | <https://raw.githubusercontent.com/openai/baselines/master/baselines/common/retro_wrappers.py> |
| SB3 | `AtariWrapper(frame_skip=4)` → `MaxAndSkipEnv(skip=4)`; `action_repeat_probability=0.0` | external `VecFrameStack(n_stack=4)` | `WarpFrame` 84×84×1 | <https://raw.githubusercontent.com/DLR-RM/stable-baselines3/master/stable_baselines3/common/atari_wrappers.py> |
| EnvPool (Atari) | `frame_skip=4`; `repeat_action_probability=0` | `stack_num=4` | `(4, 84, 84)` gray, INTER_AREA, `gray_scale=True`, `noop_max=30`, `max_episode_steps=27000`, `episodic_life=False`, `reward_clip=False`, `use_fire_reset=True` | <https://envpool.readthedocs.io/en/latest/env/atari.html> |
| Sample Factory | `env_frameskip` default **1**, Atari override **4** | `env_framestack` default **1**, Atari override **4** | `ResizeObservation(84,84)` + `GrayScaleObservation` | <https://raw.githubusercontent.com/alex-petrenko/sample-factory/master/sample_factory/cfg/cfg.py> · <https://raw.githubusercontent.com/alex-petrenko/sample-factory/master/sf_examples/atari/atari_params.py> |
| Dopamine | `AtariPreprocessing(frame_skip=4)`; `>0` **enforced** | **`NATURE_DQN_STACK_SIZE = 4`** | 84×84×1, max-pool last 2, INTER_AREA; sticky 0.25 default | <https://raw.githubusercontent.com/google/dopamine/master/dopamine/discrete_domains/atari_lib.py> |
| ACME | `action_repeats=4`, `pooled_frames=2` (**range-checked**) | `num_stacked_frames=4` | `scale_dims=(84,84)` gray default | <https://raw.githubusercontent.com/google-deepmind/acme/master/acme/wrappers/atari_wrapper.py> |
| TMRL (rtgym) | real-time clock: `time_step_duration=0.05` (**20 Hz**), `act_buf_len=2` | `IMG_HIST_LEN=4` | 64×64 gray, window 256×128 | <https://raw.githubusercontent.com/trackmania-rl/tmrl/master/tmrl/custom/tm/tm_gym_interfaces.py> · <https://raw.githubusercontent.com/yannbouteiller/rtgym/main/rtgym/envs/real_time_env.py> |
| PokemonRedExperiments | **`act_freq=24`** (button held 8) | `frame_stacks=3` | (72, 80, 3) from 144×160 | <https://raw.githubusercontent.com/PWhiddy/PokemonRedExperiments/master/v2/red_gym_env_v2.py> |
| PyBoy | manual `pyboy.tick(15)` | — | (144,160,3) raw | <https://raw.githubusercontent.com/Baekalfen/PyBoy/master/README.md> |

**Range-validation error texts (useful for a report's "what good looks like" section):**
- Dopamine: `if frame_skip <= 0: raise ValueError(f'Frame skip should be strictly positive, got {frame_skip}')`
  — valid range = strictly positive, **no upper bound**.
- ACME: `if not 1 <= pooled_frames <= action_repeats: raise ValueError("pooled_frames ({}) must be between 1 and
  action_repeats ({}) inclusive".format(pooled_frames, action_repeats))`.
- SB3: **no validation** — only `if frame_skip > 1`.
- retro / Stable-Retro: **no such parameter at all**.

### 6g. TMRL — real-time (TrackMania)

- **Correct repo: <https://github.com/trackmania-rl/tmrl>** — **739★**, last commit **May 2025**, **MIT**.
  (`tmrlproj/tmrl` → **404**.) Docs: <https://tmrl.readthedocs.io/en/latest/>.
- **Real-time loop**: README shows `for _ in range(200):  # **rtgym ensures this runs at 20Hz by default**` and
  `env.unwrapped.wait()  # rtgym-specific method to artificially 'pause' the environment when needed`.
  <https://raw.githubusercontent.com/trackmania-rl/tmrl/master/README.md>
- **Loop constants** (`RTGYM_CONFIG` in the README's `config.json`, both `TM20FULL` and `TM20LIDAR`):
  **`"time_step_duration": 0.05`** (= **20 Hz**), `"start_obs_capture": 0.04`,
  `"time_step_timeout_factor": 1.0`, `"act_buf_len": 2`, `"ep_max_length": 1000`, `"wait_on_done": true`.
  **There is no `SLEEP_TIME_MS` constant — NOT FOUND / UNVERIFIED.**
- **Screen input** (`TM20FULL`): `"WINDOW_WIDTH": 256`, `"WINDOW_HEIGHT": 128`, `"SLEEP_TIME_AT_RESET": 1.5`,
  **`"IMG_HIST_LEN": 4`**, **`"IMG_WIDTH": 64`**, **`"IMG_HEIGHT": 64`**, **`"IMG_GRAYSCALE": true`**.
- **Capture API — and this is a verified GDI instance**: `tmrl/custom/tm/utils/window.py` uses, on **Windows**,
  `win32gui`/`win32ui` **GDI `BitBlt(..., win32con.SRCCOPY)`** into `np.frombuffer(bits, dtype='uint8')` shaped
  `(h, w, 4)`; on **Linux**, `mss` `sct.grab(monitor)` plus `xdotool windowmove/windowsize`.
  <https://raw.githubusercontent.com/trackmania-rl/tmrl/master/tmrl/custom/tm/utils/window.py>
- `TM2020Interface(RealTimeGymInterface)`: `__init__(img_hist_len=4, gamepad=True, save_replays=False,
  grayscale=True, resize_to=(64,64))`; `grab_data_and_img()` does
  `img = self.window_interface.screenshot()[:, :, :3]  # BGR ordering` → `cv2.resize(img, self.resize_to)` →
  `cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)`. Observation = `[speed, gear, rpm, imgs]` with
  `Box(shape=(img_hist_len, h, w))` → **(4, 64, 64) grayscale**. Action `Box(low=-1.0, high=1.0, shape=(3,))` =
  [gas, brake, steer], via `vgamepad.VX360Gamepad`.
  <https://raw.githubusercontent.com/trackmania-rl/tmrl/master/tmrl/custom/tm/tm_gym_interfaces.py>
- **rtgym timing realism (the most valuable "real-time is hard" evidence found)** — PyPI page for rtgym v0.16:
  *"on Windows 11, a **20ms (50Hz)** target in rtgym will in fact result in… the actual duration of individual
  time steps constantly oscillates between **15ms and 31ms**. This is because the time granularity of the
  `sleep` call in Windows is **16ms**"*; *"on Linux, rtgym easily achieves a time step duration of **2ms
  (500Hz)**"*. Example `benchmarks()` output: `inference_duration (0.0141, 0.0012)`,
  `join_duration (0.0371, 0.0065)`, `get_obs_duration (8.0e-05, 1.4e-04)`,
  `send_control_duration (0.000634, 0.000524)`, `step_duration (0.0374, 0.0067)`,
  `time_step_duration (0.0514, 0.0061)`. <https://pypi.org/project/rtgym/>
- Result numbers (README): *"the superhuman target was set to about **32s** on the `tmrl-test` track, while the
  trained policy had a mean performance of about **45.5s**"*.

### 6h. Throughput numbers for vectorised screen-input RL

- **EnvPool** <https://github.com/sail-sg/envpool> — **~1.5k★** (rounded), Apache-2.0, recent commits.
  README <https://raw.githubusercontent.com/sail-sg/envpool/main/README.md>:
  *"high performance (**~1M raw FPS with Atari games, ~3M raw FPS with MuJoCo simulator on DGX-A100**)"*;
  *"**1 Million** Atari frames / **3 Million** MuJoCo steps per second simulation with 256 CPU cores, **~20x**
  throughput of Python subprocess-based vector env"*; *"**~3x** throughput… on low resource setup like 12 CPU
  cores"*; *"which is **14.9x / 19.6x** of the `gym.vector_env` baseline… On a typical PC setup with 12 CPU
  cores, EnvPool's throughput is **3.1x / 2.9x**"*.
  Atari "Highest FPS" table (Laptop(12) / Workstation(32) / TPU-VM(96) / DGX-A100(256)):
  For-loop 4,893 / 7,914 / 3,993 / 4,640; Subprocess 15,863 / 47,699 / 46,910 / 71,943;
  **Sample-Factory 28,216 / 138,847 / 222,327 / 707,494**; EnvPool sync 37,396 / 133,824 / 170,380 / 427,851;
  EnvPool async 49,439 / 200,428 / 359,559 / 891,286; EnvPool numa+async — / — / 373,169 / **1,069,922**.
  Benchmark method note: *"produced with ALE Atari environment `PongNoFrameskip-v4` (with environment wrappers
  from OpenAI Baselines)"* on a TPUv3-8 VM (96 cores, 2 NUMA) and a DGX-A100 (256 cores, 8 NUMA).
  Paper **arXiv:2206.10558** ("**one million frames per second**… **three million**… on MuJoCo… **When running
  EnvPool on a laptop, the speed is 2.8x that of the Python subprocess**… only takes **five minutes** to train
  agents to play Atari Pong and MuJoCo Ant on a laptop."). <https://arxiv.org/abs/2206.10558>
  (The README badge citation of 2212.11516 is wrong per this check.)
- **Sample Factory** <https://github.com/alex-petrenko/sample-factory> — **~1k★** (rounded), **MIT**;
  *"no Windows support at this time"*. Paper **arXiv:2006.11751** — title *"Sample Factory: Egocentric 3D
  Control from Pixels at **100000 FPS** with Asynchronous Reinforcement Learning"*; abstract *"throughput higher
  than $10^5$ environment frames/second on non-trivial control problems in 3D"*. ICML2020,
  <http://proceedings.mlr.press/v119/petrenko20a.html>. Third-party cross-check: EnvPool measures Sample-Factory
  Atari at up to **707,494 FPS** on 256 cores. Atari defaults: `env_frameskip=4, env_framestack=4`,
  `obs_scale=255.0`, `gamma=0.99`, `train_for_env_steps=10_000_000`, `batch_size=256`, `rollout=128`,
  `learning_rate=0.00025`; env order matches baselines/SB3
  (*"chosen to match Stable-Baselines3 and CleanRL implementations as precisely as possible"*).
- **Dopamine** <https://github.com/google/dopamine> — **~11k★** (rounded). Instrumentation exists
  (`average_steps_per_second = number_steps / time_delta`; TensorBoard `Train/AverageStepsPerSecond`) but
  **no absolute FPS values are published → UNVERIFIED**.
- **Atari-100k frame accounting (SPR, arXiv:2007.05929)**: *"agents are allowed only **100k steps of environment
  interaction (producing 400k frames of input)** per game, which roughly corresponds to **two hours of real-time
  experience**"*; *"equivalent to **400,000 frames**, or just under two hours – compared to the typical standard
  of **50,000,000 environment steps**, or roughly **39 days**"*.
  <https://ar5iv.labs.arxiv.org/html/2007.05929> · <https://arxiv.org/abs/2007.05929>
  SimPLe (arXiv:1903.00374): *"100k interactions… which corresponds to two hours of real-time play."*
  <https://arxiv.org/abs/1903.00374>
- **Metadata caveat**: for `stable-baselines3`, `tmrl`, `sample-factory`, `envpool`, `dopamine`, `acme` the exact
  `pushed_at`/`archived` are **UNVERIFIED (403)**; star values above are shields.io-rounded where not exact.
  Verified exactly: `openai/retro` (3,586★, 2024-02-22, **archived**), `Farama-Foundation/Stable-Retro`
  (1,078★, 2026-09-15, not archived), `trackmania-rl/tmrl` (739★).

---

## 7. Generic screen-scraping vision bots

### 7a. PyAutoGUI — the authoritative published performance numbers

**Source**: <https://pyautogui.readthedocs.io/en/latest/screenshot.html> · quickstart
<https://pyautogui.readthedocs.io/en/latest/quickstart.html>

- *"On a 1920 x 1080 screen, the `screenshot()` function takes **roughly 100 milliseconds** - it's not fast but
  it's not slow."* → **~100 ms per full-screen capture; ≤10 FPS if capture-bound.**
- *"On a 1920 x 1080 screen, the locate function calls take about **1 or 2 seconds**. This may be too slow for
  action video games, but works for most purposes and applications."*
- *"These 'locate' functions are fairly expensive; they can take a **full second** to run. The best way to speed
  them up is to pass a `region` argument."* Quickstart: *"the locate functions are slow and can take a full
  second or two."*
- *"Optionally, you can pass `grayscale=True`… to give a **slight speedup (about 30%-ish)**. This desaturates the
  color… but **potentially causing false-positive matches**."*
- `confidence`: *"The optional `confidence` keyword argument specifies the accuracy with which the function
  should locate the image on screen. This is helpful in case the function is not able to locate an image due to
  negligible pixel differences"* + *"**Note**: You need to have OpenCV installed for the `confidence` keyword to
  work."*
- Behaviour change: *"As of version **0.9.41**, if the locate functions can't find the provided image, they'll
  raise **`ImageNotFoundException`** instead of returning `None`."*
- Capture backends: Pillow required; macOS uses `screencapture`; Linux uses `scrot`.
- **Interpretation caveat**: `locateOnScreen` = `screenshot()` (~100 ms) + template load + `matchTemplate` +
  `minMaxLoc`. The 1–2 s is **dominated by capture and per-call template load, not by `matchTemplate`**. Do not
  quote it as a `matchTemplate` timing.
- **Correction**: the docs say *slow*, **not** *unreliable*. For "unreliable", cite the tracker:
  <https://github.com/asweigart/pyautogui/issues/771> ("confidence parameter not working with opencv-python
  v 4.7.0.72"; body UNVERIFIED) and
  <https://stackoverflow.com/questions/57832850/documentation-says-to-use-a-confidence-parameter-but-it-throws-an-error>.
  Which OpenCV method `confidence` maps to is **not documented → UNVERIFIED**.

### 7b. pytesseract + OpenCV HUD readers

- **No credible published accuracy/latency benchmark was found.** Representative repos expose a `REGION` and a
  binarisation `THRESHOLD` (e.g. `REGION = (100, 200, 800, 600)`, `THRESHOLD = 150`, `SAVE_DEBUG_IMAGES`) but
  publish **no metrics**: <https://github.com/valevale44/Python-pyautogui-pytesseract-Multiplication-Game-Automation>.
  → **UNVERIFIED / NOT FOUND.**
- **The one concrete non-ML OCR pipeline with exact constants is SerpentAI's** (§2c).
- **The one concrete OCR latency claim is OSBC's "as little as 2 milliseconds"** (self-reported, no hardware):
  <https://raw.githubusercontent.com/kelltom/OS-Bot-COLOR/main/README.md>

---

## 8. OpenCV `cv2.matchTemplate` — deep dive

### 8a. `mask` parameter: version history (verified from source at each tag)

| Version | Masked methods implemented | Fallback |
|---|---|---|
| 2.4.x, 3.0.0-beta | none (no `mask` argument) | — |
| 3.0.0-rc1 … 4.3.0 (and current `3.4` branch) | **`TM_SQDIFF`, `TM_CCORR_NORMED`** | `CV_Error(Error::StsNotImplemented, "")` |
| **4.4.0 → current 4.x** | **all six**: SQDIFF, SQDIFF_NORMED, CCORR, CCORR_NORMED, CCOEFF, CCOEFF_NORMED | none |

Verbatim from 4.0.0 / 4.3.0 / 3.4:
```cpp
if (method == CV_TM_SQDIFF) { ... }
else if (method == CV_TM_CCORR_NORMED) { ... }
else
    CV_Error(Error::StsNotImplemented, "");
```
URLs: <https://raw.githubusercontent.com/opencv/opencv/2.4.13.7/modules/imgproc/src/templmatch.cpp> ·
<https://raw.githubusercontent.com/opencv/opencv/3.0.0-rc1/modules/imgproc/src/templmatch.cpp> ·
<https://raw.githubusercontent.com/opencv/opencv/4.0.0/modules/imgproc/src/templmatch.cpp> ·
<https://raw.githubusercontent.com/opencv/opencv/4.3.0/modules/imgproc/src/templmatch.cpp> ·
<https://raw.githubusercontent.com/opencv/opencv/4.4.0/modules/imgproc/src/templmatch.cpp> ·
<https://raw.githubusercontent.com/opencv/opencv/4.x/modules/imgproc/src/templmatch.cpp>

**So the brief's premise is right for OpenCV 3.0.0-rc1 through 4.3.0, and obsolete from 4.4.0.** Python renders
the ≤4.3.0 failure as `cv2.error: ... (-213:The function/feature is not implemented) ... in function
'matchTemplateMask'`; the C++ source text is verified, the **exact Python-rendered string is UNVERIFIED**.

**Mask semantics also changed at 4.4.0.** 3.x–4.3.0 scaled image and template by `1.0/255` in the masked path
only (`img.convertTo(img, type, 1.0/255)`), so masked vs unmasked `TM_SQDIFF` outputs were **not on the same
scale** (a factor of ≈1.54e-5). Current 4.x uses plain `img.convertTo(img, CV_32F)`.

### 8b. Official documentation for `mask`

`docs.opencv.org` is behind a Cloudflare JS challenge (**HTTP 403** to automation). Equivalent official content
was read via the OpenCV documentation mirrors <https://docs.opencv.tw/4.11.0/df/dfb/group__imgproc__object.html>
and <https://docs.opencv.ac.cn/4.10.0/df/dfb/group__imgproc__object.html>.

**`@param mask`, verbatim:**
> *"Optional mask. It must have the same size as templ. It must either have the same number of channels as
> template or only one channel, which is then used for all template and image channels. **If the data type is
> CV_8U, the mask is interpreted as a binary mask, meaning only elements where mask is nonzero are used and are
> kept unchanged independent of the actual mask value (weight equals 1). For data type CV_32F, the mask values
> are used as weights.** The exact formulas are documented in #TemplateMatchModes."*

- **Python binding confirmed**: `cv.matchTemplate(image, templ, method[, result[, mask]]) -> result` —
  `result` precedes `mask`, so pass `mask=` by keyword.
- Result: single-channel 32F of size `(W-w+1) × (H-h+1)`. Best match via `minMaxLoc` — **global minimum for
  `TM_SQDIFF`, global maximum for `TM_CCORR`/`TM_CCOEFF`**.
- **Runtime asserts** (4.x `matchTemplateMask`): `_mask.depth() == CV_8U || CV_32F`;
  `_mask.channels() == _templ.channels() || _mask.channels() == 1`; `_templ.size() == _mask.size()`;
  image ≥ template.
- **Documentation gaps worth flagging**: the `TemplateMatchModes` enum gives "with mask" formulas for SQDIFF,
  SQDIFF_NORMED, CCORR, CCORR_NORMED and CCOEFF — but **not for `TM_CCOEFF_NORMED`** (imgproc.hpp ~line 3979),
  even though the implementation handles it. And the **C++ tutorial still prints the stale "only two methods"
  sentence** (§0.3), while the Python tutorial never mentions masks at all.
- **Complexity note: NOT FOUND.** I checked the matchTemplate documentation for OpenCV **2.3.2**
  (<https://www.opencv.org.cn/opencvdoc/2.3.2/html/modules/imgproc/doc/object_detection.html> — which also
  confirms the pre-4.x signature had **no `mask`**) and the 4.11/4.10 mirrors. **No O(·) complexity or "O(1)"
  claim is documented for `matchTemplate`.** Treat the brief's "complexity note" as **not in the docs**.

### 8c. Performance internals (verified source)

- **Direct-vs-FFT crossover**: `static bool useNaive(Size size) { int dft_size = 18; return size.height <
  dft_size && size.width < dft_size; }` → **templates under 18×18 use naive direct correlation, larger ones use
  DFT block convolution**.
- **FFT blocking constants** in `crossCorr`: `const double blockScale = 4.5; const int minBlockSize = 256;`;
  block = `cvRound(templ*4.5)` clamped to ≥ `256 - templ + 1`, DFT size via `getOptimalDFTSize`; the template
  DFT is computed once and reused across blocks.
- **Caveat on which path uses FFT**: 2.4's CPU `crossCorr` was DFT-based
  (<https://raw.githubusercontent.com/opencv/opencv/2.4/modules/imgproc/src/templmatch.cpp>). In 3.4/4.x the
  `useNaive`/`ConvolveBuf` machinery is tied to the **UMat/OpenCL** overload. **Whether the CPU `Mat` path ever
  uses the DFT routine is UNVERIFIED.**
- **Precision path**: `bool use64f = (depth == CV_8U) && (method == cv::TM_SQDIFF || method ==
  cv::TM_SQDIFF_NORMED);` → 8-bit SQDIFF accumulates in **CV_64F** (double memory traffic), then converts to 32F.
- **Fast paths that make timings machine-dependent**: IPP `ipp_matchTemplate` (bails when `img.channels() != 1`
  or `templ.size().area()*4 > img.size().area()`), OpenCL `ocl_matchTemplate` for `UMat`, and the
  `cv_hal_matchTemplate` hook. If a HAL returns OK, `TM_SQDIFF_NORMED`/`TM_CCOEFF`/`TM_CCOEFF_NORMED` **still**
  run `common_matchTemplate` — an extra normalisation pass even with acceleration.
- **Masked matching silently bypasses OpenCL and HAL**: in `cv::matchTemplate` the mask branch early-`return`s
  **before** `CV_OCL_RUN(...)` and before the HAL hook:
  ```cpp
  if (!_mask.empty()) { cv::matchTemplateMask(_img, _templ, _result, method, _mask); return; }
  ```
  → passing a mask is **pure CPU**, regardless of build.
- **Masked `TM_CCOEFF_NORMED` costs ≈ 4–5 cross-correlations** plus several full-`corrSize` temporaries.
- **Degenerate-template silent failure** (both the plain and masked paths):
  `if (templNorm < DBL_EPSILON && method == TM_CCOEFF_NORMED) { result = Scalar::all(1); return; }` and
  masked `if (norm_templx < DBL_EPSILON || cvIsNaN(norm_templx)) { result = Scalar::all(1); return; }`.
  → a flat/low-contrast template makes **every** location score 1.0 and `minMaxLoc` returns 1.0 at (0,0); a bot
  reads that as "found at top-left".
- **Rounding guards**: `if (diff2 <= std::min(0.5, 10 * FLT_EPSILON * wndSum2)) t = 0; // avoid rounding errors`
  and a saturation band `else if (fabs(num) < t*1.125) num = num > 0 ? 1 : -1;`.
- **Input assertions**: `CV_Assert( TM_SQDIFF <= method && method <= TM_CCOEFF_NORMED )`;
  `CV_Assert( (depth == CV_8U || depth == CV_32F) && type == _templ.type() && _img.dims() <= 2 )`.
- URL: <https://raw.githubusercontent.com/opencv/opencv/4.x/modules/imgproc/src/templmatch.cpp>
- The masked path is still actively bug-fixed (titles/URLs real; bodies UNVERIFIED):
  [#23694](https://github.com/opencv/opencv/pull/23694) "Update matchTemplate with mask" (merged 2023-05-27) ·
  [#23651](https://github.com/opencv/opencv/pull/23651) "Update matchTemplate with mask docstring" ·
  [#26703](https://github.com/opencv/opencv/pull/26703) "Fix matchTemplate with mask crash" (merged 2025-06-19) ·
  [#29071](https://github.com/opencv/opencv/pull/29071) "avoid NaN in masked TM_CCOEFF_NORMED for constant
  templates" · [#29677](https://github.com/opencv/opencv/pull/29677) "accept CV_Bool masks in matchTemplate"
  (merged 2026-08-11). NaN/Inf reports:
  <https://forum.opencv.org/t/java-matchtemplate-with-mask-and-tm-ccorr-normed-fails-to-find-match-infinity-nan/18445> ·
  <https://forum.opencv.org/t/inf-values-in-template-matching/6167>

### 8d. Measured `matchTemplate` numbers (the only ones found)

**SO 60097895** — "openCV cv::matchTemplate running twice slower on a 'better/newer' intel cpu"
(score 0, **0 answers**):
- `haystack` **64×64** crop, `needle` **12×12** crop, `TM_CCOEFF_NORMED`, `nbmatch = 10000`, MSVC VS2019 (v142),
  `#pragma optimize("", off)`, OpenCV built from source →
  **i9-7920X = 0.28 ms/match; i7-9700K = 0.14 ms/match**.
- Real application (sizes **not stated → UNVERIFIED**): **i7-9700K 1 ms; i7-6800K 1.3 ms; i9-7920X 2.8 ms;
  i9-9820X 2.8 ms**; an edit notes 2.8 ms → **1.9 ms** when yielding at 100% load (frequency-scaling artefact).
- <https://stackoverflow.com/questions/60097895/opencv-cvmatchtemplate-running-twice-slower-on-a-better-newer-intel-cpu>

**Published ms for a 1920×1080 image + ~50×50 template with `TM_CCOEFF_NORMED`: NOT FOUND → UNVERIFIED.**
Likewise **method-vs-method and grayscale-vs-color measured ratios: NOT FOUND → UNVERIFIED.** Structural
expectations only: cost scales ~linearly with channel count (docs); `TM_SQDIFF*` on 8-bit uses 64F accumulators;
normalised variants add passes; masked CCOEFF_NORMED adds ≈4–5 passes; masked → CPU-only.
**No first-party benchmark was produced** (no local `cv2` available in either research session).

### 8e. Scale-pyramid recipes and cost

**Canonical recipe** — PyImageSearch, *"Multi-scale Template Matching using Python and OpenCV"*
<https://pyimagesearch.com/2015/01/26/multi-scale-template-matching-using-python-opencv/> (2015-01-26; targets
"Python 2.7/Python 3.4+ and OpenCV 2.4.X"):

```python
template = cv2.Canny(cv2.cvtColor(cv2.imread(args["template"]), cv2.COLOR_BGR2GRAY), 50, 200)
(tH, tW) = template.shape[:2]
for scale in np.linspace(0.2, 1.0, 20)[::-1]:
    resized = imutils.resize(gray, width=int(gray.shape[1] * scale))
    r = gray.shape[1] / float(resized.shape[1])
    if resized.shape[0] < tH or resized.shape[1] < tW:
        break
    edged = cv2.Canny(resized, 50, 200)
    result = cv2.matchTemplate(edged, template, cv2.TM_CCOEFF)
    (_, maxVal, _, maxLoc) = cv2.minMaxLoc(result)
    if found is None or maxVal > found[0]:
        found = (maxVal, maxLoc, r)
(_, maxLoc, r) = found
(startX, startY) = (int(maxLoc[0] * r), int(maxLoc[1] * r))
```
Key specifics:
- **It scales the IMAGE, not the template** — `np.linspace(0.2, 1.0, 20)[::-1]`, i.e. 20 scales from **100% down
  to 20%** of the image (not `linspace(0.5, 1.0, …)`).
- Both template and search image are reduced to **Canny edges (50, 200)** before matching — *"applying template
  matching using edges rather than the raw image gives us a substantial boost in accuracy"*.
- Method is **`cv2.TM_CCOEFF`, NOT `TM_CCOEFF_NORMED`** ⇒ **no [0,1] score and no usable absolute threshold**;
  it is a global-argmax competition only.
- **Early exit** `if resized.shape[0] < tH or resized.shape[1] < tW: break`; **global max across scales**;
  coordinates multiplied back by `r`.
- Article explicitly disclaims rotation/non-affine: *"template matching is not ideal if you are trying to match
  rotated objects or objects that exhibit non-affine transformations."*
- **NO timings/FPS/ms are published in the article** → any ms claim attributed to it would be fabricated.

**Documented real-world failure of this exact recipe** — SO 55884829 ("Multi-scale Template Matching in
real-time", accepted answer): the user's `found` was **always `None`**. Answer verbatim: *"The template you use
is 743x887 pixels, taller than it is wide… if you're not using a 1080p fullHD camera, the output of the camera
you're using is probably already smaller than the template (in height). This breaks the loop and causes `found`
to be `None`."* → a size mismatch silently produces **zero** detections rather than an error.
<https://stackoverflow.com/questions/55884829/multi-scale-template-matching-in-real-time>

**Cost vs number of scales** — **no published ms numbers → UNVERIFIED**. A **derived estimate (analysis, not a
measurement)**: shrinking the *image* means per-scale cost ∝ s², so total ≈ `Σ sᵢ²`; for 20 uniform s ∈ [0.2, 1.0],
`mean(s²) = ∫₀.₂¹s²ds/0.8 ≈ 0.413`, giving **≈ 8.3× one full-resolution call — not 20×**. Resizing the
*template upward* against a fixed full-resolution image costs ≈ N × full-resolution instead, i.e. far worse at
equal scale count. Label as analysis.

**Rotation/scale-aware alternative with a published API (no published timings)**: SRL's `FindDTMRotated` /
`FindDTMRotatedSE` / `FindDTMRotatedAlternating` and `Walker.ScaledSearch`/`ScaleMinimap`
(§3d) — <https://villavu.github.io/SRL-Development/wrappers.html> ·
<https://villavu.github.io/SRL-Development/walker.html>

### 8f. Failure modes of naive template matching in games

| Failure mode | Evidence |
|---|---|
| **Scale mismatch → confident false match** | PyImageSearch Figure 1 and the article's premise: a substantially smaller template yields a false detection. <https://pyimagesearch.com/2015/01/26/multi-scale-template-matching-using-python-opencv/> |
| **Scale mismatch → silent zero detections** | SO 55884829: template taller than camera output → `break` → `found is None` forever. <https://stackoverflow.com/questions/55884829/multi-scale-template-matching-in-real-time> |
| **Rotation / non-affine transforms** | *"template matching is not ideal if you are trying to match rotated objects"* — PyImageSearch URL above |
| **Constant / low-variance template** | `TM_CCOEFF_NORMED` returns an **all-ones** result map for a (near-)zero-variance template — every pixel "matches" — for both the plain and masked paths. `templmatch.cpp` (§8c) |
| **Anti-aliasing / font rendering affects detection** | WowClassicGrindBot README recommends replacing the default WoW font with a bolder one for *"big improvement to the NpcNameFinder component"*. <https://raw.githubusercontent.com/Xian55/WowClassicGrindBot/main/README.md> |
| **Overlay/UI occlusion of the template** | ChromaScape: *"For stacked/banked items (that have numbers over it) you need to crop out the **top 10 pixels**."* <https://github-wiki-see.page/m/StaticSweep/ChromaScape/wiki/Making-your-first-script> |
| **Transparency / alpha** | OSBC claims a *modified* OpenCV matcher *"even supporting images with transparency"* <https://raw.githubusercontent.com/kelltom/OS-Bot-COLOR/main/README.md>. OpenCV's answer is the `mask` parameter. **Verified flags**: `IMREAD_COLOR` (=1, default) *"always convert image to the 3 channel BGR color image"* → **alpha dropped**; `IMREAD_UNCHANGED` (=-1) *"return the loaded image as is (with alpha channel, otherwise it gets cropped)"* → required to get alpha. <https://raw.githubusercontent.com/opencv/opencv/4.x/modules/imgcodecs/include/opencv2/imgcodecs.hpp> · <https://docs.opencv.org/4.x/d8/d6a/group__imgcodecs__flags.html>. **The "alpha is dropped without compositing" detail is UNVERIFIED** (docs state only the conversion). **Trap:** a `CV_8U` alpha mask is **binarised**, so anti-aliased edge alpha (1…254) becomes **full weight 1** — expect soft edges only with a `CV_32F` mask in [0,1]. A 4-channel BGRA mask is rejected outright (`channels == templ.channels() || 1`) — split alpha out first. And in ≤4.3.0/3.4, masking with `TM_CCOEFF_NORMED` **raised `StsNotImplemented`**, so "mask the sprite edges and use the normalised-coefficient method" was impossible before 4.4.0. |
| **Threshold-convention confusion** | `TM_CCOEFF_NORMED` is maximised; `TM_SQDIFF*` minimised. Real code uses both ends: **OpenCV's own Python tutorial** uses `threshold = 0.8` with `TM_CCOEFF_NORMED` on a **Mario game screenshot** (§8g); Botty requires *"Scores should be well above **0.9**"* (<https://raw.githubusercontent.com/johannes-do/botty/master/README.md>); ChromaScape uses **0.05 preferred / 0.15 max** with *"a lower threshold means that the match needs to be more accurate"* (<https://github-wiki-see.page/m/StaticSweep/ChromaScape/wiki/Making-your-first-script>); Ivan Guerra's colorbot computes an **area-scaled SSD threshold** `w*h*15.0²` (<https://raw.githubusercontent.com/ivan-guerra/colorbot/master/src/vision.rs>); PyImageSearch's multi-scale recipe uses `TM_CCOEFF` so **no absolute threshold is possible at all**. |
| **Animated elements / flicker** | Not documented for template matching itself. The RL world solves it by **max-pooling over 2 frames** (baselines `MaxAndSkipEnv`, §6a) and Machado et al. name *"frame pooling… to reduce visual artifacts"* (§6e). Screen-bot equivalents: `SRL.IsAnimating`/`GetPixelShift` (<https://villavu.github.io/SRL-Development/pixelshift.html>), pygta5 `motion_detection` (§1a), SerpentAI `GameFrame.compare_ssim` (§2b). |
| **Multiple peaks / no NMS** | See §8g — OpenCV's own tutorial acknowledges `minMaxLoc` returns only one location and performs **no** non-maximum suppression. |
| **DPI / UI-scale changes** | **UNVERIFIED — no primary source found.** Closest documented analogues: WowClassicGrindBot's mandatory `Render Scale 100%` + sharpening-off settings, and OSBC's requirement not to change UI scale/theme before capturing templates. |
| **Naive pixel search too slow** | SerpentAI's own source: `# TODO: Optimize for ms gain` above an exact-RGB `np.where` scan. <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/sprite.py> |
| **Whole-screen search cost** | PyAutoGUI: locate calls ~**1–2 s** on 1920×1080; `region=` is the documented fix; `grayscale=True` gives *"about 30%-ish"* but risks false positives. §7a |

### 8g. Thresholding / multiple peaks — OpenCV's own published example

From the official **Python** tutorial (verbatim):
> *"Suppose you are searching for an object which has multiple occurrences, **cv.minMaxLoc()** won't give you all
> the locations. In that case, we will use thresholding. So in this example, we will use a screenshot of the
> famous game **Mario** and we will find the coins in it."*
```python
res = cv.matchTemplate(img_gray, template, cv.TM_CCOEFF_NORMED)
threshold = 0.8
loc = np.where(res >= threshold)
for pt in zip(*loc[::-1]):
    cv.rectangle(img_rgb, pt, (pt[0] + w, pt[1] + h), (0, 0, 255), 2)
```
- **`threshold = 0.8` with `TM_CCOEFF_NORMED`, on a game screenshot, is the officially published constant.**
- The vendor states the `minMaxLoc` limitation itself: *"cv.minMaxLoc() won't give you all the locations."*
- **The tutorial performs no non-maximum suppression** — one rectangle per thresholded pixel, producing
  near-duplicate clusters (visible in its own result image). A bot taking "the" match from thresholded pixels
  needs its own NMS/dedupe, which the tutorial does not provide.
- URLs: <https://raw.githubusercontent.com/opencv/opencv/4.x/doc/py_tutorials/py_imgproc/py_template_matching/py_template_matching.markdown> ·
  <https://docs.opencv.org/4.x/d4/dc6/tutorial_py_template_matching.html>

### 8h. HUD detection via HSV `inRange` + `findContours` — published accuracy/latency

**UNVERIFIED / NOT FOUND.** No peer-reviewed paper, preprint, or credible repo README was located that publishes
**both accuracy and latency** (with hardware) for game-HUD detection via `cv2.inRange` HSV thresholds +
`findContours`. Sources that surfaced are off-domain or unquantified:
- arXiv 2504.17943 (threshold segmentation, but dairy-calf body weight) <https://arxiv.org/html/2504.17943v1>
- arXiv 2112.02162 (agricultural robot, Prior-Otsu HSV range) <https://ar5iv.labs.arxiv.org/html/2112.02162>
- `Exaphis/TensorFlOW` — Overwatch HP bars, **ML not HSV+contours**, no verified numbers
  <https://github.com/Exaphis/TensorFlOW>
- A CSDN colour-space comparison using `cv2.inRange(y, 40, 220)`, no HUD benchmark
  <https://blog.csdn.net/weixin_30466039/article/details/98694960>
- `inRange`/`findContours` complexity notes: **UNVERIFIED**.

**Honest statement for the report**: the colour-threshold + contour HUD pipeline is the de-facto default in this
niche (ChromaScape, SRL `FindColorsTolerance`/`CTS0-2`, RuneDark "Color-on-Color OCR", OSBC *"color detection…
to locate objects/NPCs outlined by solid colors"*), but **no primary source publishes accuracy or FPS figures for
it.** The only quantitative claims nearby are OSBC's self-reported **2 ms** OCR latency and PyBoy's emulator
throughput.

---

## 9. Cross-cutting technique → implementation map

| Technique | Concrete verified implementations |
|---|---|
| **ROI restriction** | pygta5 `roi()` + `fillPoly`/`bitwise_and` (§1a); pyautogui `region=` (§7a); SerpentAI `extract_region_from_image` + `screen_region` in `SpriteLocator`/`click_screen_region` (§2c/2d); ChromaScape zones + `ScreenManager.captureZone` (§3b); SRL `Minimap.GetZoomRectangle` (§3d); TMRL `move_and_resize` to a fixed window (§6g) |
| **Resolution downscale** | pygta5 1920×1120 → **480×270**; reboot 1280×720 → **480×270** (§1); SerpentAI half/quarter/eighth + 100×100 `ssim_frame` (§2b); PokémonRedExperiments 144×160 → **72×80**; TMRL → **64×64**; RL canonical → **84×84 gray** |
| **Frame skipping / action repeat** | baselines `MaxAndSkipEnv(skip=4)`; SB3 `frame_skip=4`; EnvPool `frame_skip=4`; Sample Factory Atari `env_frameskip=4`; Dopamine `frame_skip=4`; ACME `action_repeats=4`; PokémonRedExperiments `act_freq=24` (hold 8); PyBoy `tick(15)`; SerpentAI RECORD at **10 FPS** |
| **Frame stacking** | baselines `FrameStack(env, 4)` + `LazyFrames`; Dopamine `NATURE_DQN_STACK_SIZE = 4`; ACME `num_stacked_frames=4`; EnvPool `stack_num=4`; PokémonRedExperiments `frame_stacks=3` via `np.roll`; TMRL `IMG_HIST_LEN=4`; SerpentAI `GameFrameBuffer(size=5)` |
| **Masked template matching** | OpenCV `mask` (2 methods ≤4.3.0; all six ≥4.4.0) — §8a/8b; OSBC's transparency claim — §3c |
| **Multi-scale templates** | PyImageSearch 20-step image pyramid on edge maps (§8e); SRL `FindDTMRotated*` + `Walker.ScaledSearch` (§3d) |
| **Edge-map matching instead of raw pixels** | PyImageSearch Canny 50/200 on template **and** image (§8e); pygta5 Canny 200/300 lanes (§1a) |
| **Confirm before act** | SerpentAI `click_sprite`/`click_string` → `False`; `SpriteIdentifier` → `"UNKNOWN"`; pygta5 25-frame stuck confirmation; ChromaScape "click until red click" + XP-change gate; SRL `DidRedClick`/`DidYellowClick` |
| **Action cooldowns / frame-count timing** | Botty `casting_frames`; WowClassicGrindBot `PressDuration=50 ms`, `DelayAfterCast=1450 ms`; ChromaScape `waitRandomMillis(80-100 / 300 / 600-800 / 650-800)`; colorbot JSON `delay`; rtgym `time_step_duration=0.05` |
| **Cached / precomputed artifacts** | SerpentAI lazily-cached GameFrame variants + cached sprite signatures/constellations; OpenCV computes the template DFT once per `crossCorr` and reuses it across blocks; ChromaScape pre-downloads OSBC/SRL glyph templates |
| **Randomised click distribution inside a detection** | colorbot random point in matched rect; ChromaScape `ClickDistribution` + `tightness`; SRL `RandomPoint`, `Inventory.RandomSlotNearby`; pygta5 random steering blends |
| **Mouse parking / not hijacking the cursor** | ChromaScape RemoteInput virtual second mouse; SRL `Mouse.Idle`, `Mouse.Miss`, `Walker.DoMouseAhead` |
| **Humanised motion** | WindMouse (colorbot, ChromaScape); Bezier (OSBC); SerpentAI 20-step `interp1d` interpolation; SRL `GaussRand`/`TruncatedGauss`/`SkewedRand` |
| **Anti-detection breaks** | ChromaScape 1% chance of a 2–5 min break; SRL `Antiban.AddSleep/AddBreak/AddTask` |
| **Focus gating** | SerpentAI hard pause when unfocused + `force` kwarg; SRL `RSClient.LoseFocus` |

---

## 10. Every measured number gathered, with source

| Number | Meaning | Source |
|---|---|---|
| **~100 ms** | pyautogui `screenshot()` on 1920×1080 | <https://pyautogui.readthedocs.io/en/latest/screenshot.html> |
| **1–2 s** ("a full second") | pyautogui `locateOnScreen` on 1920×1080 — capture-dominated, **not** a matchTemplate timing | same |
| **~30%** | pyautogui speedup from `grayscale=True` (risks false positives) | same |
| **0.28 ms / 0.14 ms** | `TM_CCOEFF_NORMED`, 64×64 haystack, 12×12 needle — i9-7920X / i7-9700K | <https://stackoverflow.com/questions/60097895/opencv-cvmatchtemplate-running-twice-slower-on-a-better-newer-intel-cpu> |
| **1 / 1.3 / 2.8 / 2.8 ms** | same, "real application" (sizes unstated → UNVERIFIED) | same |
| **0.8** | OpenCV's own tutorial threshold for `TM_CCOEFF_NORMED` (Mario screenshot) | <https://raw.githubusercontent.com/opencv/opencv/4.x/doc/py_tutorials/py_imgproc/py_template_matching/py_template_matching.markdown> |
| **"well above 0.9"** | Botty template-match acceptance score | <https://raw.githubusercontent.com/johannes-do/botty/master/README.md> |
| **0.05 preferred, 0.15 max** | ChromaScape image-match threshold (lower = stricter) | <https://github-wiki-see.page/m/StaticSweep/ChromaScape/wiki/Making-your-first-script> |
| **2 ms** | OSBC "locate text on screen" (self-reported, no hardware) | <https://raw.githubusercontent.com/kelltom/OS-Bot-COLOR/main/README.md> |
| **×124 / ×344 / ×395 realtime** | PyBoy: full rendering / `tick(15)` frame-skip / no rendering | <https://raw.githubusercontent.com/Baekalfen/PyBoy/master/README.md> |
| **6,000 fps at ×100; 395 h gameplay/h; 3160 h in 1 h on 8 cores** | PyBoy README conversions | same |
| **~1M / ~3M raw FPS** | EnvPool Atari / MuJoCo on DGX-A100 (256 cores) | <https://raw.githubusercontent.com/sail-sg/envpool/main/README.md> |
| **14.9× / 19.6×** | EnvPool vs `gym.vector_env` (Atari / MuJoCo, 256 cores); **3.1× / 2.9×** on 12 cores | same |
| **707,494 FPS** | Sample-Factory Atari, third-party measurement, 256 cores | same |
| **1,069,922 FPS** | EnvPool numa+async Atari, 256 cores | same |
| **100000 FPS** | Sample Factory paper title/abstract claim (3D pixels, non-trivial control) | <https://arxiv.org/abs/2006.11751> |
| **20 Hz nominal; actual 15–31 ms steps on Windows 11; 2 ms achievable on Linux** | rtgym real-time loop granularity (Windows `sleep` granularity = 16 ms) | <https://pypi.org/project/rtgym/> |
| **0.0514 s ± 0.0061** | rtgym example `time_step_duration` benchmark; `step_duration 0.0374` | same |
| **100k steps = 400k frames ≈ 2 h** | Atari-100k protocol frame accounting (SPR) | <https://ar5iv.labs.arxiv.org/html/2007.05929> |
| **200M frames ≈ 38 days** | standard Atari training budget (Machado et al.) | <https://ar5iv.labs.arxiv.org/html/1709.06009> |
| **30 FPS (10 FPS recording)** | SerpentAI default agent frame rate | <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/game.py> |
| **150 frames (~5 s @30 fps)** | SerpentAI Redis buffer the consumer waits for | <https://raw.githubusercontent.com/SerpentAI/SerpentAI/dev/serpent/frame_grabber.py> |
| **90 FPS / 3×30 FPS / 1280×720 → 480×270** | pygta5 reboot rendering, per-camera FPS, downscale | <https://raw.githubusercontent.com/Sentdex/pygta5/master/project_info/cameras.md> |
| **~40 FPS cap** | pygta5 lane keeper via `cv2.waitKey(25)` | <https://pythonprogramming.net/self-driving-car-python-plays-gta-v/> |
| **frame_skip/stacks: 4/4** | canonical Atari preprocessing across baselines, SB3, EnvPool, SF, Dopamine, ACME | §6f URLs |
| **act_freq 24, hold 8, stacks 3, obs 72×80×3** | PokémonRedExperiments V2 | <https://raw.githubusercontent.com/PWhiddy/PokemonRedExperiments/master/v2/red_gym_env_v2.py> |
| **≈1.05e11 total timesteps; 64 envs; ep_length 163,840** | PokémonRedExperiments V2 training scale | <https://raw.githubusercontent.com/PWhiddy/PokemonRedExperiments/master/v2/baseline_fast_v2.py> |
| **18 px; blockScale 4.5; minBlockSize 256** | OpenCV `useNaive` crossover and `crossCorr` FFT blocking | §8c |
| **20 scales, 0.2→1.0; ≈8.3× one full-res call (analysis)** | multi-scale pyramid and its derived cost | §8e |
| **TOLERANCE 3; MAX_ATTEMPTS 1000; MIN_EDGE_DISTANCE 10.0; MAX_PIXEL_VARIANCE 15.0** | colorbot constants | <https://raw.githubusercontent.com/ivan-guerra/colorbot/master/src/vision.rs> |
| **tightness 15.0 / tolerance 15; poll 300 ms; break 120–300 s at 1%** | ChromaScape constants | <https://github-wiki-see.page/m/StaticSweep/ChromaScape/wiki/Intermediate-Scripting:-From-Planning-to-Execution> |
| **PressDuration 50 ms; DelayAfterCast 1450 ms** | WowClassicGrindBot defaults | <https://raw.githubusercontent.com/Xian55/WowClassicGrindBot/main/README.md> |

**No verified ms-per-`matchTemplate`-call benchmark exists for a 1920×1080 search image.** Measure on target
hardware.

---

## 11. Explicit UNVERIFIED / open items

1. The exact PR/commit that added `mask` in the OpenCV 3.0.0 cycle (window **3.0.0-beta → 3.0.0-rc1** is verified).
2. The exact PR that extended masked matching to all six methods in **4.4.0** (the 4.3.0→4.4.0 boundary **is**
   verified).
3. The Python-rendered `StsNotImplemented` text for ≤4.3.0 masked `TM_CCOEFF_NORMED`.
4. Any complexity table / "O(1)" note for `matchTemplate` — I believe none exists in the docs.
5. Measured ms for 1920×1080 + ~50×50 `TM_CCOEFF_NORMED`; method-vs-method and grayscale-vs-color measured
   ratios; per-scale timings for the multi-scale loop.
6. Whether the **CPU `Mat`** `crossCorr` path ever uses the DFT routine (the `dft_size=18` / `blockScale=4.5` /
   `minBlockSize=256` constants are verified to exist in the OpenCL overload).
7. **DPI/UI-scale**, **anti-aliasing/resampling**, animated elements, particle effects, lighting-change citations
   — nothing verified.
8. `IMREAD_COLOR`'s compositing behaviour for transparent pixels.
9. **Section 8h in its entirety** — no credible published accuracy/latency for HSV `inRange` + `findContours` HUD
   detection; also no complexity notes for `inRange`/`findContours`.
10. **Published pytesseract HUD-OCR accuracy** — not found.
11. **Capture API (GDI/DXGI/mss/X11) for every Diablo/WoW repo** in §5 — unverified.
12. **FPS/ms numbers for any Diablo/WoW/OSRS-colour-bot repo** — none published.
13. **`openai/retro` / `stable-retro` "224×224 RGB by default"** — not documented; shape is a per-game crop.
14. **DQN Nature 2015** exact wording (nature.com auth redirect; DeepMind PDF unsupported content type) —
   corroborated only indirectly via Dopamine's docstring.
15. `retro`'s own docs/blog "flickering" wording — the danieltakeshi blog cited by SB3's docstring **failed to
    fetch**; the flicker rationale is nonetheless explicit in baselines' max-pool comment and in Machado et al.
16. **Exact `pushed_at` / `archived` for `stable-baselines3`, `trackmania-rl/tmrl`, `sample-factory`, `envpool`,
    `dopamine`, `acme`** — GitHub API 403; star counts are shields.io-rounded. Verified exactly: `openai/retro`,
    `Farama-Foundation/Stable-Retro`, `trackmania-rl/tmrl` (739★).
17. **Stars / `pushed_at` for `kelltom/OS-Bot-COLOR`, `Villavu/SRL-Development`, `cemenenkoff/runedark-public`,
    `Xian55/WowClassicGrindBot`, `aeon0/botty`** — API 403 (**not** 404): they very likely exist; only metadata
    is missing. A re-run with `GITHUB_TOKEN` would close this.
18. `PWhiddy/PokemonRedExperiments` "completed the game" — **not in the README**; only *"Reaches Cerulean"*.
19. `ACME`'s `max_episode_len` defaults to `None` (*"there is no maximum length"*); the often-cited ACME
    27000-step default is **not** in `atari_wrapper.py` → UNVERIFIED. (27000 is verified for Dopamine's `Runner`
    and EnvPool's `max_episode_steps`.)
20. Sample Factory's metrics doc contains an internally inconsistent sentence about
    `perf/_sample_throughput` vs `perf/_fps` (quoted verbatim in §6e); treat as an apparent doc bug.
