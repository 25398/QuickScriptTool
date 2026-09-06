/**
 * ui/bridge.js — JS ↔ C++ WebView2 bridge
 */
(function () {
  function post(msg) {
    const payload = typeof msg === "string" ? msg : JSON.stringify(msg);
    if (window.qstBridge && typeof window.qstBridge.post === "function") {
      window.qstBridge.post(payload);
      return;
    }
    if (window.chrome && chrome.webview && chrome.webview.postMessage) {
      chrome.webview.postMessage(payload);
    }
  }

  window.qst = {
    post,
    listScripts: () => post({ type: "listScripts" }),
    listRecordings: () => post({ type: "listRecordings" }),
    listAgentConversations: () => post({ type: "listAgentConversations" }),
    runScript: (id) => post({ type: "runScript", id: String(id || "") }),
    stopScript: () => post({ type: "stopScript" }),
    startClicker: (opts) => post(Object.assign({ type: "startClicker" }, opts || {})),
    stopClicker: () => post({ type: "stopClicker" }),
    getClickerStatus: () => post({ type: "getClickerStatus" }),
    openSettingsData: () => post({ type: "openSettingsData" }),
    saveSettings: (settings) => post({ type: "saveSettings", settings: settings || {} }),
    restoreSettingsDefaults: () => post({ type: "restoreSettingsDefaults" }),
    openEditor: (path) => post({ type: "openEditor", path: String(path || "") }),
    peekScriptActions: (path, reqId) =>
      post({
        type: "peekScriptActions",
        path: String(path || ""),
        reqId: String(reqId || ""),
      }),
    previewScriptActions: (path, reqId, max) =>
      post({
        type: "previewScriptActions",
        path: String(path || ""),
        reqId: String(reqId || ""),
        max: Number(max) || 16,
      }),
    saveEditor: (payload) => post(Object.assign({ type: "saveEditor" }, payload || {})),
    debugScript: (payload) => post(Object.assign({ type: "debugScript" }, payload || {})),
    deleteScript: (path) => post({ type: "deleteScript", path: String(path || "") }),
    renameScript: (path, name) =>
      post({ type: "renameScript", path: String(path || ""), name: String(name || "") }),
    importScript: (kind) => post({ type: "importScript", kind: String(kind || "macro") }),
    exportScript: (path) => post({ type: "exportScript", path: String(path || "") }),
    openRecordingOptimize: (path, name) =>
      post({
        type: "openRecordingOptimize",
        path: String(path || ""),
        name: String(name || ""),
      }),
    setScriptHotkey: (opts) => post(Object.assign({ type: "setScriptHotkey" }, opts || {})),
    setRecorderMode: (mode) => post({ type: "setRecorderMode", mode: Number(mode) || 0 }),
    openAgentConversation: (id, createNew, opts) =>
      post({
        type: "openAgentConversation",
        id: String(id || ""),
        createNew: createNew ? 1 : 0,
        peek: opts && opts.peek ? 1 : 0,
        panelKey: String((opts && opts.panelKey) || ""),
      }),
    openAgentWindow: (opts) =>
      post(Object.assign({ type: "openAgentWindow" }, opts || {})),
    sendAgentMessage: (opts) => post(Object.assign({ type: "sendAgentMessage" }, opts || {})),
    saveAgentDraft: (opts) => post(Object.assign({ type: "agentSaveDraft" }, opts || {})),
    cancelAgentMessage: () => post({ type: "cancelAgentMessage" }),
    listAgentChanges: () => post({ type: "listAgentChanges" }),
    revertAgentChange: (id) =>
      post({ type: "revertAgentChange", id: String(id || "") }),
    pasteAgentClipboard: () => post({ type: "pasteAgentClipboard" }),
    saveClipboardImage: (dataUrl, ext) =>
      post({ type: "saveClipboardImage", dataUrl: String(dataUrl || ""), ext: String(ext || "jpg") }),
    saveImageAs: (opts) =>
      post({
        type: "saveImageAs",
        path: String((opts && opts.path) || ""),
        dataUrl: String((opts && opts.dataUrl) || ""),
      }),
    deleteAgentConversation: (id) =>
      post({ type: "deleteAgentConversation", id: String(id || "") }),
    getGlobalHotkey: () => post({ type: "getGlobalHotkey" }),
    setGlobalHotkey: (opts) => post(Object.assign({ type: "setGlobalHotkey" }, opts || {})),
    setTypingHotkeysMuted: (muted) =>
      post({ type: "setTypingHotkeysMuted", muted: muted ? 1 : 0 }),
    beginHotkeyCapture: (payload) =>
      post(Object.assign({ type: "beginHotkeyCapture" }, payload || {})),
    endHotkeyCapture: () => post({ type: "endHotkeyCapture" }),
    captureActionKey: (payload) =>
      post(Object.assign({ type: "captureActionKey" }, payload || {})),
    formatHotkey: (payload) =>
      post(Object.assign({ type: "formatHotkey" }, payload || {})),
    captureGlobalHotkey: (payload) =>
      post(Object.assign({ type: "captureGlobalHotkey" }, payload || {})),
    setMode: (mode) => post({ type: "window.setMode", mode: String(mode || "home") }),
    setHomeSize: (w, h, opts) =>
      post({
        type: "window.setHomeSize",
        w: Number(w) || 1552,
        h: Number(h) || 960,
        center: opts && opts.center === false ? 0 : 1,
      }),
    listLibraryFolders: (kind) =>
      post({ type: "listLibraryFolders", kind: String(kind || "macro") }),
    createLibraryFolder: (kind, folder) =>
      post({ type: "createLibraryFolder", kind: String(kind || "macro"), folder: String(folder || "") }),
    renameLibraryFolder: (kind, folder, name) =>
      post({
        type: "renameLibraryFolder",
        kind: String(kind || "macro"),
        folder: String(folder || ""),
        name: String(name || ""),
      }),
    deleteLibraryFolder: (kind, folder) =>
      post({ type: "deleteLibraryFolder", kind: String(kind || "macro"), folder: String(folder || "") }),
    moveScriptToFolder: (path, folder) =>
      post({ type: "moveScriptToFolder", path: String(path || ""), folder: String(folder || "") }),
    setItemLibraryFolder: (kind, id, folder) =>
      post({
        type: "setItemLibraryFolder",
        kind: String(kind || "macro"),
        id: String(id || ""),
        folder: String(folder || ""),
      }),
    modeReady: () => post({ type: "window.modeReady" }),
    uncloak: () => post({ type: "window.uncloak" }),
    setActiveHomeTab: (tab) =>
      post({ type: "setActiveHomeTab", tab: Number(tab) || 0 }),
    setHomeSelection: (tab, path) =>
      post({
        type: "setHomeSelection",
        tab: Number(tab) || 0,
        path: String(path || ""),
      }),
    getHomeState: () => post({ type: "getHomeState" }),
    startRecord: (opts) =>
      post(Object.assign({ type: "startRecord" }, opts || {})),
    stopRecord: () => post({ type: "stopRecord" }),
    openScheduledTasks: () => post({ type: "openScheduledTasks" }), // HTML only; no forceNative in product UI
    listScheduledTasks: () => post({ type: "listScheduledTasks" }),
    saveScheduledTask: (opts) => post(Object.assign({ type: "saveScheduledTask" }, opts || {})),
    deleteScheduledTask: (id) => post({ type: "deleteScheduledTask", id: String(id || "") }),
    setScheduledTasksGlobalDisabled: (disabled) =>
      post({ type: "setScheduledTasksGlobalDisabled", disabled: disabled ? 1 : 0 }),
    loadOptimizeRecording: (path) => post({ type: "loadOptimizeRecording", path: String(path || "") }),
    applyOptimizeRecording: (opts) =>
      post(Object.assign({ type: "applyOptimizeRecording" }, opts || {})),
    pickScreenRegion: () => post({ type: "pickScreenRegion" }),
    getEngineStatus: () => post({ type: "getEngineStatus" }),
    getAppBranding: () => post({ type: "getAppBranding" }),
    checkUpgrade: () => post({ type: "checkUpgrade" }),
    themeCatalog: () => post({ type: "themeCatalog" }),
    applyTheme: (opts) => post(Object.assign({ type: "applyTheme" }, opts || {})),
    openThemeCustom: (opts) => post(Object.assign({ type: "openThemeCustom" }, opts || {})),
    crosshairPick: (mode) => post({ type: "crosshairPick", mode: String(mode || "coordinates") }),
    browsePath: (executableOnly) =>
      post({ type: "browsePath", executableOnly: executableOnly ? 1 : 0 }),
    installDriver: (kind, opts) =>
      post({
        type: "installDriver",
        kind: String(kind || ""),
        uninstall: opts && opts.uninstall ? 1 : 0,
      }),
    queryVhidStatus: () => post({ type: "queryVhidStatus" }),
    systemReboot: (opts) =>
      post({ type: "systemReboot", fw: opts && opts.fw ? 1 : 0 }),
    installOcr: (repair) => post({ type: "installOcr", repair: repair ? 1 : 0 }),
    showDebugWindow: () => post({ type: "showDebugWindow" }),
    debugWindowClosed: () => post({ type: "debugWindowClosed" }),
    debugWindowSetTopmost: (topmost) =>
      post({ type: "debugWindowSetTopmost", topmost: topmost ? 1 : 0 }),
    showWindowModePreview: (opts) =>
      post(Object.assign({ type: "showWindowModePreview" }, opts || {})),
    hideWindowModePreview: () => post({ type: "hideWindowModePreview" }),
    pickImageFile: () => post({ type: "pickImageFile" }),
    findImageCrop: (opts) => post(Object.assign({ type: "findImageCrop" }, opts || {})),
    findImageMatch: (opts) => post(Object.assign({ type: "findImageMatch" }, opts || {})),
    captureTemplateScreenshot: () => post({ type: "captureTemplateScreenshot" }),
    readImageDataUrl: (path, reqId) =>
      post({
        type: "readImageDataUrl",
        path: String(path || ""),
        reqId: String(reqId || ""),
      }),
    resolveImagePath: (path) => post({ type: "resolveImagePath", path: String(path || "") }),
    captureScriptHotkey: (opts) =>
      post(Object.assign({ type: "captureScriptHotkey" }, opts || {})),
    testOcr: (opts) => post(Object.assign({ type: "testOcr" }, opts || {})),
    minimize: () => post({ type: "window.minimize" }),
    close: () => post({ type: "window.close" }),
    drag: () => post({ type: "window.drag" }),
  };

  if (window.chrome && chrome.webview && chrome.webview.addEventListener) {
    chrome.webview.addEventListener("message", (ev) => {
      const data = ev.data;
      let obj = data;
      if (typeof data === "string") {
        try { obj = JSON.parse(data); } catch (_) { return; }
      }
      window.dispatchEvent(new CustomEvent("qst:bridge", { detail: obj }));
    });
  }

  function wireChrome() {
    const bar = document.querySelector(".app > .titlebar");
    if (bar) {
      bar.addEventListener("mousedown", (e) => {
        if (e.button !== 0) return;
        if (e.target.closest(".win-btn, button, a, input, .win-btns")) return;
        post({ type: "window.drag" });
      });
    }
    // 覆盖层对话框标题栏拖动（含录制优化全窗 setMode opt）
    document.addEventListener(
      "mousedown",
      (e) => {
        if (e.button !== 0) return;
        if (document.documentElement.classList.contains("agent-shell")) return;
        const title = e.target.closest && e.target.closest(".dlg-title");
        if (!title) return;
        if (e.target.closest(".win-btn, button, a, input, .win-btns")) return;
        post({ type: "window.drag" });
      },
      true
    );
    document.querySelectorAll(".app > .titlebar .win-btn").forEach((btn) => {
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        const id = btn.id || "";
        if (id === "btnClose" || btn.classList.contains("close")) {
          // 宏编辑器：右上角 × 等同「取消」，回主界面（勿直接关进程）
          const inEditor =
            document.body.classList.contains("editor-open") ||
            !!(document.getElementById("app") &&
              document.getElementById("app").classList.contains("editor-mode"));
          if (inEditor) {
            window.dispatchEvent(new CustomEvent("qst:exit-editor"));
            return;
          }
          post({ type: "window.close" });
        } else if (id === "btnMin")
          post({ type: "window.minimize" });
        else if (id === "btnSettings")
          window.dispatchEvent(new CustomEvent("qst:open-settings"));
      });
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", wireChrome);
  } else {
    wireChrome();
  }
})();
