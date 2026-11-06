(() => {
  "use strict";

  const byId = (id) => document.getElementById(id);
  const ui = Object.fromEntries([
    "connection-badge", "connection-label", "notice", "notice-text", "dismiss-notice",
    "live-label", "live-preview", "preview-overlay", "overlay-title", "overlay-detail",
    "frame-label", "frame-age", "resolution-label", "fps-label", "capture-button",
    "capture-button-label", "capture-hint", "service-state", "freshness-state", "uptime-label",
    "storage-label", "service-error", "photo-selection-label", "selected-empty", "selected-content",
    "open-photo", "selected-image", "selected-id", "selected-time", "selected-specs", "selected-frame",
    "download-photo", "delete-photo", "gallery-count", "gallery-empty", "photo-gallery", "retention-note",
    "photo-dialog", "close-dialog", "dialog-title", "dialog-image", "dialog-caption", "dialog-download",
    "settings-form", "settings-fields", "setting-resolution", "setting-fps", "setting-quality", "quality-value",
    "apply-settings", "settings-current", "settings-message", "settings-version",
    "check-quality", "quality-message", "quality-results", "quality-binding", "quality-mean", "quality-stddev",
    "quality-dark", "quality-bright", "quality-sharpness",
    "image-controls-state", "image-controls-unavailable", "image-controls-content", "image-controls-readback",
    "exposure-form", "exposure-fields", "exposure-mode", "exposure-time", "exposure-gain", "exposure-time-range",
    "exposure-gain-range", "actual-exposure-time", "actual-exposure-gain", "exposure-converged", "apply-exposure", "exposure-message",
    "white-balance-form", "white-balance-fields", "white-balance-mode", "wb-rgain", "wb-grgain", "wb-gbgain", "wb-bgain",
    "actual-white-balance", "white-balance-range", "apply-white-balance", "white-balance-message",
  ].map((id) => [id, byId(id)]));
  const model = {
    status: null, online: false, lastStatusAt: 0, statusError: "", captures: [], selectedId: null,
    capturing: false, deleting: false, mutationVersion: 0, listBusy: false, lastListAttempt: 0,
    statusBusy: false, streamOpen: false, streamHealthy: false, streamStartedAt: 0,
    streamGeneration: 0, streamFailures: 0, active: false, disposed: false,
    settings: null, settingsBusy: false, settingsApplying: false, settingsDirty: false,
    settingsAwaitingStatus: false, settingsEpoch: 0, lastSettingsAttempt: 0,
    settingsMessage: "", settingsMessageError: false,
    qualityBusyId: null, qualityResult: null, qualityError: "", qualityGeneration: 0,
    imageControls: null, imageControlsBusy: false, imageControlsApplying: null, imageControlsError: "",
    imageControlsEpoch: 0, lastImageControlsAttempt: 0, imageControlsReadAt: 0,
    exposureDirty: false, whiteBalanceDirty: false, exposureMessage: "", exposureMessageError: false,
    whiteBalanceMessage: "", whiteBalanceMessageError: false,
  };
  const reads = new Set();
  const requests = new Set();
  let statusTimer = null;
  let presentationTimer = null;
  let streamTimer = null;
  let noticeTimer = null;
  let qualityController = null;
  const wbGainKeys = ["rgain", "grgain", "gbgain", "bgain"];

  function setText(element, value) {
    const text = String(value);
    if (element.textContent !== text) element.textContent = text;
  }

  function showNotice(message, error = false) {
    clearTimeout(noticeTimer);
    setText(ui["notice-text"], message);
    ui.notice.dataset.tone = error ? "error" : "success";
    ui.notice.hidden = false;
    noticeTimer = setTimeout(() => { ui.notice.hidden = true; }, error ? 12000 : 6500);
  }

  async function request(path, { method = "GET", timeout = 5000, read = true, body, headers, controller: suppliedController } = {}) {
    const controller = suppliedController || new AbortController();
    requests.add(controller);
    if (read) reads.add(controller);
    let timedOut = false;
    const timer = setTimeout(() => { timedOut = true; controller.abort(); }, timeout);
    try {
      const response = await fetch(path, { method, body, headers, cache: "no-store", credentials: "same-origin", signal: controller.signal });
      let payload;
      try { payload = await response.json(); } catch { throw new Error("服务返回了无法读取的结果，请稍后重试。"); }
      if (!payload || typeof payload !== "object" || Array.isArray(payload)) throw new Error("服务返回的结果格式无效。");
      if (!response.ok) throw new Error(typeof payload.error === "string" ? payload.error : `请求未成功（${response.status}）。`);
      return payload;
    } catch (error) {
      if (timedOut) throw new Error("请求超时，请检查RK连接后重试。");
      if (error.name === "AbortError") throw error;
      if (error instanceof TypeError) throw new Error("无法连接RK，请检查网络和相机服务。");
      throw error;
    } finally {
      clearTimeout(timer);
      reads.delete(controller);
      requests.delete(controller);
    }
  }

  function number(value, fallback = 0) {
    return Number.isFinite(value) && value >= 0 ? value : fallback;
  }

  function normalizeSettings(value) {
    const positiveInteger = (entry) => Number.isInteger(entry) && entry > 0;
    if (!value || !positiveInteger(value.width) || !positiveInteger(value.height)
        || !positiveInteger(value.preview_fps) || !Number.isInteger(value.jpeg_quality)
        || value.jpeg_quality < 50 || value.jpeg_quality > 98
        || !Array.isArray(value.profiles) || !Array.isArray(value.preview_fps_options)) {
      throw new Error("板端返回的图像参数格式无效。");
    }
    const profiles = value.profiles.filter((profile) => profile && positiveInteger(profile.width) && positiveInteger(profile.height))
      .map((profile) => ({ width: profile.width, height: profile.height, label: typeof profile.label === "string" ? profile.label : `${profile.width} × ${profile.height}` }));
    const frameRates = [...new Set(value.preview_fps_options.filter(positiveInteger))];
    if (!profiles.length || !frameRates.length) throw new Error("板端未提供可用的分辨率或预览帧率。");
    return { ...value, profiles, preview_fps_options: frameRates };
  }

  function settingsDraft() {
    const profile = model.settings?.profiles.find((entry) => `${entry.width}x${entry.height}` === ui["setting-resolution"].value);
    const fps = Number(ui["setting-fps"].value);
    const quality = Number(ui["setting-quality"].value);
    if (!profile || !model.settings.preview_fps_options.includes(fps) || !Number.isInteger(quality) || quality < 50 || quality > 98) return null;
    return { width: profile.width, height: profile.height, preview_fps: fps, jpeg_quality: quality };
  }

  function setSettingsMessage(message, error = false) {
    model.settingsMessage = message;
    model.settingsMessageError = error;
    renderSettings();
  }

  function fillSettingsInputs(settings, preserveDraft) {
    const draft = preserveDraft && model.settingsDirty ? {
      resolution: ui["setting-resolution"].value, fps: ui["setting-fps"].value, quality: ui["setting-quality"].value,
    } : null;
    const resolutionOptions = settings.profiles.map((profile) => {
      const option = document.createElement("option");
      option.value = `${profile.width}x${profile.height}`;
      option.textContent = profile.label;
      return option;
    });
    ui["setting-resolution"].replaceChildren(...resolutionOptions);
    const fpsOptions = settings.preview_fps_options.map((fps) => {
      const option = document.createElement("option");
      option.value = String(fps);
      option.textContent = `${fps} 帧/秒`;
      return option;
    });
    ui["setting-fps"].replaceChildren(...fpsOptions);
    ui["setting-resolution"].value = draft ? draft.resolution : `${settings.width}x${settings.height}`;
    ui["setting-fps"].value = draft ? draft.fps : String(settings.preview_fps);
    ui["setting-quality"].value = draft ? draft.quality : String(settings.jpeg_quality);
    updateSettingsDirty();
  }

  function updateSettingsDirty() {
    const draft = settingsDraft();
    model.settingsDirty = Boolean(draft && model.settings && ["width", "height", "preview_fps", "jpeg_quality"].some((key) => draft[key] !== model.settings[key]));
    renderSettings();
  }

  function isReconfiguring() {
    return model.settingsApplying || model.settingsAwaitingStatus || model.status?.reconfiguring === true;
  }

  function renderSettings() {
    const settings = model.settings;
    const changing = isReconfiguring();
    ui["settings-fields"].disabled = !settings || changing;
    ui["apply-settings"].disabled = !settings || !model.settingsDirty || changing || model.capturing || model.deleting
      || Boolean(model.imageControlsApplying) || !model.online || Date.now() - model.lastStatusAt > 3500;
    ui["apply-settings"].setAttribute("aria-busy", String(model.settingsApplying));
    setText(ui["apply-settings"], changing ? "正在调整…" : "应用参数");
    setText(ui["quality-value"], settings ? ui["setting-quality"].value : "—");
    setText(ui["settings-version"], settings ? `参数版本 ${number(settings.settings_version)}` : "正在读取");
    const actual = model.status && Number.isFinite(model.status.settings_version)
      && (!settings || model.status.settings_version >= number(settings.settings_version)) ? model.status : settings;
    setText(ui["settings-current"], actual ? `当前实际：${number(actual.width)} × ${number(actual.height)} · 预览 ${number(actual.preview_fps)} 帧/秒 · JPEG压缩质量 ${number(actual.jpeg_quality)}` : "正在读取当前实际参数。");
    const message = model.settingsMessage || (model.settingsDirty ? "有尚未应用的修改，点击“应用参数”后生效。" : "");
    ui["settings-message"].hidden = !message;
    ui["settings-message"].dataset.tone = model.settingsMessageError ? "error" : "success";
    setText(ui["settings-message"], message);
  }

  async function refreshSettings() {
    if (!model.active || model.settingsBusy || model.settingsApplying) return;
    model.settingsBusy = true;
    model.lastSettingsAttempt = Date.now();
    const epoch = model.settingsEpoch;
    try {
      const settings = normalizeSettings(await request("/api/settings"));
      if (!model.active || epoch !== model.settingsEpoch) return;
      model.settings = settings;
      fillSettingsInputs(settings, true);
      if (settings.error) setSettingsMessage(String(settings.error), true);
      else if (model.settingsMessage.startsWith("参数读取失败：")) setSettingsMessage("");
    } catch (error) {
      if (error.name !== "AbortError" && model.active) setSettingsMessage(`参数读取失败：${error.message}`, true);
    } finally {
      model.settingsBusy = false;
      renderSettings();
    }
  }

  async function applySettings(event) {
    event.preventDefault();
    if (ui["apply-settings"].disabled) return;
    const draft = settingsDraft();
    if (!draft) { setSettingsMessage("请选择板端提供的分辨率、帧率和有效JPEG质量。", true); return; }
    model.settingsApplying = true;
    model.settingsAwaitingStatus = true;
    model.settingsEpoch += 1;
    model.imageControlsEpoch += 1;
    stopStream();
    setSettingsMessage("正在切换采集参数，等待相机恢复新画面。已有照片会保留。");
    renderStatus();
    try {
      const body = new URLSearchParams(Object.entries(draft).map(([key, value]) => [key, String(value)]));
      const settings = normalizeSettings(await request("/api/settings", {
        method: "POST", body, headers: { "Content-Type": "application/x-www-form-urlencoded" }, timeout: 30000, read: false,
      }));
      if (model.disposed) return;
      model.settings = settings;
      fillSettingsInputs(settings, false);
      setSettingsMessage("参数已应用，正在确认新画面。已有照片保持原样。");
      showNotice(`已应用 ${settings.width} × ${settings.height}，预览 ${settings.preview_fps} 帧/秒，JPEG压缩质量 ${settings.jpeg_quality}。`);
    } catch (error) {
      if (error.name !== "AbortError" && !model.disposed) {
        setSettingsMessage(`参数未应用：${error.message} 将重新读取实际设置。`, true);
        showNotice(`参数调整未完成：${error.message}`, true);
      }
    } finally {
      model.settingsApplying = false;
      model.settingsEpoch += 1;
      model.imageControlsEpoch += 1;
      model.lastImageControlsAttempt = 0;
      model.lastSettingsAttempt = 0;
      renderStatus();
      if (model.active) {
        void refreshSettings();
        clearTimeout(statusTimer);
        if (!model.statusBusy) void pollStatus();
      }
    }
  }

  function normalizeImageControls(value) {
    if (!value || typeof value.connected !== "boolean") throw new Error("图像调节服务返回的状态格式无效。");
    if (!value.connected) return { connected: false, error: typeof value.error === "string" ? value.error : "图像调节服务暂时无法连接。" };
    const exposure = value.exposure, wb = value.white_balance;
    const finite = (entry) => Number.isFinite(entry);
    const positive = (entry) => finite(entry) && entry > 0;
    if (!exposure || !wb || !["auto", "manual"].includes(exposure.mode) || !["auto", "manual"].includes(wb.mode)
        || !positive(exposure.time_min_ms) || !positive(exposure.time_max_ms) || exposure.time_max_ms < exposure.time_min_ms
        || !positive(exposure.gain_min) || !positive(exposure.gain_max) || exposure.gain_max < exposure.gain_min
        || !positive(exposure.manual_time_ms) || !positive(exposure.manual_gain)
        || !finite(exposure.actual_time_ms) || exposure.actual_time_ms < 0 || !finite(exposure.actual_gain) || exposure.actual_gain < 0
        || !positive(wb.gain_min) || !positive(wb.gain_max) || wb.gain_max < wb.gain_min
        || wbGainKeys.some((key) => !positive(wb[key]))) {
      throw new Error("曝光或白平衡的读回值、单位范围不完整，暂不开放操作。");
    }
    return value;
  }

  function controlNumber(value) {
    return value.toLocaleString("zh-CN", { maximumFractionDigits: 4 });
  }

  function sameNumber(left, right) {
    return Number.isFinite(left) && Number.isFinite(right) && Math.abs(left - right) <= 1e-8 * Math.max(1, Math.abs(left), Math.abs(right));
  }

  function exposureDraft() {
    const current = model.imageControls?.exposure;
    const mode = ui["exposure-mode"].value;
    if (!current || !["auto", "manual"].includes(mode)) return null;
    if (mode === "auto") return { group: "exposure", mode };
    const time = ui["exposure-time"].valueAsNumber, gain = ui["exposure-gain"].valueAsNumber;
    if (!Number.isFinite(time) || !Number.isFinite(gain) || time < current.time_min_ms || time > current.time_max_ms
        || gain < current.gain_min || gain > current.gain_max) return null;
    return { group: "exposure", mode, time_ms: time, gain };
  }

  function whiteBalanceDraft() {
    const current = model.imageControls?.white_balance;
    const mode = ui["white-balance-mode"].value;
    if (!current || !["auto", "manual"].includes(mode)) return null;
    if (mode === "auto") return { group: "white_balance", mode };
    const gains = Object.fromEntries(wbGainKeys.map((key) => [key, ui[`wb-${key}`].valueAsNumber]));
    if (wbGainKeys.some((key) => !Number.isFinite(gains[key]) || gains[key] < current.gain_min || gains[key] > current.gain_max)) return null;
    return { group: "white_balance", mode, ...gains };
  }

  function updateImageControlsDirty() {
    const controls = model.imageControls;
    if (controls?.connected) {
      const exposure = controls.exposure, wb = controls.white_balance;
      model.exposureDirty = ui["exposure-mode"].value !== exposure.mode || (ui["exposure-mode"].value === "manual"
        && (!sameNumber(ui["exposure-time"].valueAsNumber, exposure.manual_time_ms) || !sameNumber(ui["exposure-gain"].valueAsNumber, exposure.manual_gain)));
      model.whiteBalanceDirty = ui["white-balance-mode"].value !== wb.mode || (ui["white-balance-mode"].value === "manual"
        && wbGainKeys.some((key) => !sameNumber(ui[`wb-${key}`].valueAsNumber, wb[key])));
    }
    renderImageControls();
  }

  function fillImageControlsInputs(controls, appliedGroup = null) {
    const exposure = controls.exposure, wb = controls.white_balance;
    const preserveExposure = model.exposureDirty && appliedGroup !== "exposure";
    const preserveWb = model.whiteBalanceDirty && appliedGroup !== "white_balance";
    ui["exposure-time"].min = String(exposure.time_min_ms);
    ui["exposure-time"].max = String(exposure.time_max_ms);
    ui["exposure-gain"].min = String(exposure.gain_min);
    ui["exposure-gain"].max = String(exposure.gain_max);
    if (!preserveExposure) {
      ui["exposure-mode"].value = exposure.mode;
      ui["exposure-time"].value = String(exposure.manual_time_ms);
      ui["exposure-gain"].value = String(exposure.manual_gain);
    }
    for (const key of wbGainKeys) {
      ui[`wb-${key}`].min = String(wb.gain_min);
      ui[`wb-${key}`].max = String(wb.gain_max);
      if (!preserveWb) ui[`wb-${key}`].value = String(wb[key]);
    }
    if (!preserveWb) ui["white-balance-mode"].value = wb.mode;
    updateImageControlsDirty();
  }

  function renderImageControls() {
    const controls = model.imageControls;
    const connected = controls?.connected === true;
    const error = model.imageControlsError || (!connected && controls?.error) || "";
    const busy = Boolean(model.imageControlsApplying);
    const available = connected && !error;
    const paused = busy || isReconfiguring() || model.capturing || !model.online || Date.now() - model.lastStatusAt > 3500;
    ui["image-controls-content"].hidden = !connected;
    ui["image-controls-unavailable"].hidden = available;
    setText(ui["image-controls-unavailable"], error || "正在连接图像调节服务，确认可用能力后显示操作项。");
    setText(ui["image-controls-state"], busy ? "正在应用…" : available ? `已连接 · 版本 ${number(controls.version)}` : "暂不可用");
    ui["exposure-fields"].disabled = !available || paused;
    ui["white-balance-fields"].disabled = !available || paused;
    const autoExposure = ui["exposure-mode"].value === "auto";
    ui["exposure-time"].disabled = autoExposure;
    ui["exposure-gain"].disabled = autoExposure;
    const autoWb = ui["white-balance-mode"].value === "auto";
    for (const key of wbGainKeys) ui[`wb-${key}`].disabled = autoWb;
    const exposureValid = Boolean(exposureDraft()), wbValid = Boolean(whiteBalanceDraft());
    ui["apply-exposure"].disabled = !available || paused || !model.exposureDirty || !exposureValid;
    ui["apply-white-balance"].disabled = !available || paused || !model.whiteBalanceDirty || !wbValid;
    ui["apply-exposure"].setAttribute("aria-busy", String(model.imageControlsApplying === "exposure"));
    ui["apply-white-balance"].setAttribute("aria-busy", String(model.imageControlsApplying === "white_balance"));
    setText(ui["apply-exposure"], model.imageControlsApplying === "exposure" ? "正在应用曝光…" : "应用曝光");
    setText(ui["apply-white-balance"], model.imageControlsApplying === "white_balance" ? "正在应用白平衡…" : "应用白平衡");
    const exposureInvalid = model.exposureDirty && !exposureValid;
    const wbInvalid = model.whiteBalanceDirty && !wbValid;
    const exposureMessage = model.exposureMessage || (exposureInvalid ? "请按板端范围填写手动曝光时间和增益。" : model.exposureDirty ? "曝光有未应用的修改。" : "");
    const wbMessage = model.whiteBalanceMessage || (wbInvalid ? "请按板端范围填写四路白平衡增益。" : model.whiteBalanceDirty ? "白平衡有未应用的修改。" : "");
    ui["exposure-message"].hidden = !exposureMessage;
    ui["white-balance-message"].hidden = !wbMessage;
    ui["exposure-message"].dataset.tone = model.exposureMessageError || exposureInvalid ? "error" : "success";
    ui["white-balance-message"].dataset.tone = model.whiteBalanceMessageError || wbInvalid ? "error" : "success";
    setText(ui["exposure-message"], exposureMessage);
    setText(ui["white-balance-message"], wbMessage);
    if (!connected) return;
    const exposure = controls.exposure, wb = controls.white_balance;
    setText(ui["actual-exposure-time"], `${controlNumber(exposure.actual_time_ms)} 毫秒`);
    setText(ui["actual-exposure-gain"], `${controlNumber(exposure.actual_gain)} 倍`);
    setText(ui["exposure-converged"], exposure.mode === "manual" ? "当前为手动" : exposure.converged ? "自动调节已稳定" : "自动调节中");
    setText(ui["exposure-time-range"], `${exposure.time_min_ms}～${exposure.time_max_ms} 毫秒`);
    setText(ui["exposure-gain-range"], `${exposure.gain_min}～${exposure.gain_max} 倍`);
    setText(ui["actual-white-balance"], `当前读回：红 ${controlNumber(wb.rgain)} · 绿1 ${controlNumber(wb.grgain)} · 绿2 ${controlNumber(wb.gbgain)} · 蓝 ${controlNumber(wb.bgain)} 倍`);
    setText(ui["white-balance-range"], `每路可设 ${controlNumber(wb.gain_min)}～${controlNumber(wb.gain_max)} 倍`);
    const stamp = model.imageControlsReadAt ? new Date(model.imageControlsReadAt).toLocaleTimeString("zh-CN", { hour12: false }) : "—";
    setText(ui["image-controls-readback"], `最近读回 ${stamp}；每10秒更新实际读数，保留未应用的编辑。读回值不保证与当前预览属于同一帧。`);
  }

  async function refreshImageControls() {
    if (!model.active || model.imageControlsBusy || model.imageControlsApplying || isReconfiguring()) return;
    model.imageControlsBusy = true;
    model.lastImageControlsAttempt = Date.now();
    const epoch = model.imageControlsEpoch;
    try {
      const controls = normalizeImageControls(await request("/api/image-controls", { timeout: 20000 }));
      if (!model.active || epoch !== model.imageControlsEpoch) return;
      if (controls.connected && model.imageControls?.connected) {
        if (controls.exposure.mode !== model.imageControls.exposure.mode) {
          model.exposureMessage = "";
          model.exposureMessageError = false;
        }
        if (controls.white_balance.mode !== model.imageControls.white_balance.mode) {
          model.whiteBalanceMessage = "";
          model.whiteBalanceMessageError = false;
        }
      }
      model.imageControls = controls;
      model.imageControlsError = "";
      model.imageControlsReadAt = Date.now();
      if (controls.connected) fillImageControlsInputs(controls);
    } catch (error) {
      if (error.name !== "AbortError" && model.active) model.imageControlsError = `图像调节暂不可用：${error.message}`;
    } finally {
      model.imageControlsBusy = false;
      renderImageControls();
    }
  }

  async function applyImageControls(event, group) {
    event.preventDefault();
    const exposureGroup = group === "exposure";
    if (ui[exposureGroup ? "apply-exposure" : "apply-white-balance"].disabled) return;
    const draft = exposureGroup ? exposureDraft() : whiteBalanceDraft();
    if (!draft) return;
    const messageKey = exposureGroup ? "exposureMessage" : "whiteBalanceMessage";
    const errorKey = exposureGroup ? "exposureMessageError" : "whiteBalanceMessageError";
    model.imageControlsApplying = group;
    model.imageControlsEpoch += 1;
    model[messageKey] = "正在设置并读取结果，实时预览继续。";
    model[errorKey] = false;
    renderStatus();
    try {
      const body = new URLSearchParams(Object.entries(draft).map(([key, value]) => [key, String(value)]));
      const controls = normalizeImageControls(await request("/api/image-controls", {
        method: "POST", body, headers: { "Content-Type": "application/x-www-form-urlencoded" }, timeout: 20000, read: false,
      }));
      if (model.disposed) return;
      if (!controls.connected) throw new Error(controls.error || "图像调节服务未确认读回结果。");
      if (controls[group].mode !== draft.mode) throw new Error("读回模式与请求不一致，请查看实际值后重试。");
      model.imageControls = controls;
      model.imageControlsError = "";
      model.imageControlsReadAt = Date.now();
      fillImageControlsInputs(controls, group);
      model[messageKey] = `${exposureGroup ? "曝光" : "白平衡"}设置已读回，当前为${draft.mode === "auto" ? "自动" : "手动"}模式。请核对实际值与画面。`;
      showNotice(`${exposureGroup ? "曝光" : "白平衡"}设置已读回。`);
    } catch (error) {
      if (error.name !== "AbortError" && !model.disposed) {
        model[messageKey] = `应用未完成：${error.message} 将重新读取实际设置。`;
        model[errorKey] = true;
      }
    } finally {
      model.imageControlsApplying = null;
      model.imageControlsEpoch += 1;
      model.lastImageControlsAttempt = 0;
      renderStatus();
      if (model.active) void refreshImageControls();
    }
  }

  function sizeLabel(bytes) {
    const value = number(bytes);
    return value >= 1048576 ? `${(value / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(value / 1024))} KB`;
  }

  function dateLabel(value, timeOnly = false) {
    const date = new Date(value);
    if (!Number.isFinite(date.getTime())) return "时间未提供";
    return date.toLocaleString("zh-CN", {
      ...(timeOnly ? {} : { month: "2-digit", day: "2-digit" }),
      hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false,
    });
  }

  function uptimeLabel(seconds) {
    const total = Math.floor(number(seconds));
    if (total < 60) return `${total} 秒`;
    if (total < 3600) return `${Math.floor(total / 60)} 分 ${total % 60} 秒`;
    return `${Math.floor(total / 3600)} 小时 ${Math.floor(total % 3600 / 60)} 分`;
  }

  function photoUrl(value) {
    if (typeof value !== "string") throw new Error("照片地址缺失。");
    const url = new URL(value, window.location.href);
    if (url.origin !== window.location.origin || !url.pathname.startsWith("/api/captures/") || !url.pathname.endsWith(".jpg")) {
      throw new Error("服务返回了无效的照片地址。");
    }
    return url.href;
  }

  function normalizePhoto(value) {
    if (!value || typeof value.id !== "string" || !value.id || value.id.length > 256) throw new Error("照片编号无效。");
    return {
      id: value.id, frame_id: number(value.frame_id), received_at: typeof value.received_at === "string" ? value.received_at : "",
      width: number(value.width), height: number(value.height), bytes: number(value.bytes),
      url: photoUrl(value.url), download_url: photoUrl(value.download_url),
    };
  }

  function selectedPhoto() {
    return model.captures.find((photo) => photo.id === model.selectedId) || null;
  }

  function clearQuality() {
    model.qualityGeneration += 1;
    if (qualityController) qualityController.abort();
    qualityController = null;
    model.qualityBusyId = null;
    model.qualityResult = null;
    model.qualityError = "";
    renderQuality();
  }

  function renderQuality() {
    const photo = selectedPhoto();
    const busy = Boolean(photo && model.qualityBusyId === photo.id);
    const result = photo && model.qualityResult?.capture_id === photo.id ? model.qualityResult : null;
    ui["check-quality"].disabled = !photo || Boolean(model.qualityBusyId) || model.capturing || model.deleting || !model.online;
    ui["check-quality"].setAttribute("aria-busy", String(busy));
    setText(ui["check-quality"], busy ? "正在检查…" : "检查画面");
    ui["quality-message"].hidden = !photo || (!busy && !model.qualityError);
    ui["quality-message"].dataset.tone = model.qualityError ? "error" : "waiting";
    setText(ui["quality-message"], model.qualityError || (busy ? "正在独立分析这张照片，实时预览继续。" : ""));
    ui["quality-results"].hidden = !result;
    if (!result) {
      for (const id of ["quality-binding", "quality-mean", "quality-stddev", "quality-dark", "quality-bright", "quality-sharpness"]) setText(ui[id], "");
      return;
    }
    setText(ui["quality-binding"], `${result.capture_id} · ${result.width} × ${result.height} · ${Math.round(result.elapsed_ms)} ms`);
    setText(ui["quality-mean"], result.mean_brightness.toFixed(2));
    setText(ui["quality-stddev"], result.brightness_stddev.toFixed(2));
    setText(ui["quality-dark"], `${result.dark_percent.toFixed(2)}%`);
    setText(ui["quality-bright"], `${result.bright_percent.toFixed(2)}%`);
    setText(ui["quality-sharpness"], result.sharpness_score.toFixed(2));
  }

  function normalizeQuality(value, photo) {
    const metrics = ["mean_brightness", "brightness_stddev", "dark_percent", "bright_percent", "sharpness_score", "elapsed_ms"];
    if (!value || value.capture_id !== photo.id || value.frame_id !== photo.frame_id
        || value.width !== photo.width || value.height !== photo.height
        || metrics.some((key) => !Number.isFinite(value[key]) || value[key] < 0)
        || value.mean_brightness > 255 || value.dark_percent > 100 || value.bright_percent > 100
        || !Array.isArray(value.histogram) || value.histogram.length !== 256
        || value.histogram.some((count) => !Number.isSafeInteger(count) || count < 0)
        || value.histogram.reduce((sum, count) => sum + count, 0) !== photo.width * photo.height) {
      throw new Error("画面检查结果与这张照片不一致，未显示该结果。");
    }
    return value;
  }

  async function checkPhotoQuality() {
    const photo = selectedPhoto();
    if (!photo || ui["check-quality"].disabled) return;
    clearQuality();
    const generation = model.qualityGeneration;
    const controller = new AbortController();
    qualityController = controller;
    model.qualityBusyId = photo.id;
    renderQuality();
    try {
      const result = normalizeQuality(await request(`/api/captures/${encodeURIComponent(photo.id)}/quality`, {
        method: "POST", timeout: 10000, read: false, controller,
      }), photo);
      if (model.disposed || generation !== model.qualityGeneration || model.selectedId !== photo.id) return;
      model.qualityResult = result;
    } catch (error) {
      if (error.name !== "AbortError" && !model.disposed && generation === model.qualityGeneration) model.qualityError = error.message;
    } finally {
      if (generation === model.qualityGeneration) {
        model.qualityBusyId = null;
        qualityController = null;
        renderQuality();
      }
    }
  }

  function closePhotoDialog() {
    if (ui["photo-dialog"].open) ui["photo-dialog"].close();
    ui["dialog-image"].removeAttribute("src");
    ui["dialog-download"].removeAttribute("href");
  }

  function renderSelection() {
    const photo = selectedPhoto();
    ui["selected-empty"].hidden = Boolean(photo);
    ui["selected-content"].hidden = !photo;
    setText(ui["photo-selection-label"], photo ? "原始 JPEG" : "尚未选择");
    if (!photo) {
      ui["selected-image"].removeAttribute("src");
      ui["download-photo"].removeAttribute("href");
      closePhotoDialog();
      renderQuality();
      return;
    }
    if (ui["selected-image"].src !== photo.url) ui["selected-image"].src = photo.url;
    ui["selected-image"].alt = `现场照片 ${photo.id}`;
    setText(ui["selected-id"], photo.id);
    setText(ui["selected-time"], dateLabel(photo.received_at));
    ui["selected-time"].title = photo.received_at;
    setText(ui["selected-specs"], `${photo.width} × ${photo.height} / ${sizeLabel(photo.bytes)}`);
    setText(ui["selected-frame"], `#${photo.frame_id}`);
    ui["download-photo"].href = photo.download_url;
    ui["download-photo"].download = `${photo.id}.jpg`;
    ui["delete-photo"].disabled = model.deleting || model.capturing;
    setText(ui["delete-photo"], model.deleting ? "正在删除…" : "删除照片");
    renderQuality();
  }

  function renderGallery() {
    const fragment = document.createDocumentFragment();
    for (const photo of model.captures) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "photo-card";
      button.dataset.photoId = photo.id;
      button.setAttribute("aria-pressed", String(photo.id === model.selectedId));
      button.setAttribute("aria-label", `查看照片 ${photo.id}，板端接收时间 ${dateLabel(photo.received_at)}`);
      const image = document.createElement("img");
      image.src = photo.url;
      image.alt = "";
      image.loading = "lazy";
      image.decoding = "async";
      const details = document.createElement("span");
      details.className = "photo-card-info";
      const id = document.createElement("span");
      id.className = "photo-card-id";
      id.textContent = photo.id;
      id.title = photo.id;
      const time = document.createElement("span");
      time.className = "photo-card-time";
      time.textContent = dateLabel(photo.received_at, true);
      details.append(id, time);
      button.append(image, details);
      fragment.append(button);
    }
    ui["photo-gallery"].replaceChildren(fragment);
    ui["gallery-empty"].hidden = model.captures.length > 0;
    setText(ui["gallery-count"], model.captures.length);
    renderSelection();
    renderStatus();
  }

  function updateGallerySelection() {
    for (const button of ui["photo-gallery"].querySelectorAll("button[data-photo-id]")) {
      button.setAttribute("aria-pressed", String(button.dataset.photoId === model.selectedId));
    }
    renderSelection();
  }

  async function refreshCaptures() {
    if (!model.active || model.listBusy) return;
    model.listBusy = true;
    model.lastListAttempt = Date.now();
    const version = model.mutationVersion;
    try {
      const payload = await request("/api/captures");
      if (!model.active || version !== model.mutationVersion) return;
      if (!Array.isArray(payload.captures)) throw new Error("照片列表格式无效。");
      const photos = payload.captures.map(normalizePhoto);
      if (new Set(photos.map((photo) => photo.id)).size !== photos.length) throw new Error("照片列表中出现重复编号。");
      const changed = JSON.stringify(photos) !== JSON.stringify(model.captures);
      model.captures = photos;
      if (!selectedPhoto()) {
        closePhotoDialog();
        clearQuality();
        model.selectedId = photos[0]?.id || null;
      }
      if (changed) renderGallery();
    } catch (error) {
      if (error.name !== "AbortError" && model.active) showNotice(`照片列表暂时无法更新：${error.message}`, true);
    } finally {
      model.listBusy = false;
    }
  }

  function stopStream() {
    clearTimeout(streamTimer);
    streamTimer = null;
    model.streamGeneration += 1;
    model.streamOpen = false;
    model.streamHealthy = false;
    ui["live-preview"].onerror = null;
    ui["live-preview"].onload = null;
    ui["live-preview"].removeAttribute("src");
  }

  function scheduleStreamRetry() {
    if (!model.active || streamTimer !== null) return;
    const delay = Math.min(10000, 2500 * Math.max(1, model.streamFailures));
    streamTimer = setTimeout(() => { streamTimer = null; startStream(); }, delay);
  }

  function startStream() {
    if (!model.active || model.streamOpen || !model.online) return;
    const status = model.status;
    if (!status || status.state !== "ready" || !status.fresh || isReconfiguring()) return;
    const generation = ++model.streamGeneration;
    model.streamOpen = true;
    model.streamHealthy = false;
    model.streamStartedAt = Date.now();
    ui["live-preview"].onload = () => {
      if (generation !== model.streamGeneration || !model.active) return;
      model.streamHealthy = true;
      model.streamFailures = 0;
      renderStatus();
    };
    ui["live-preview"].onerror = () => {
      if (generation !== model.streamGeneration || !model.active) return;
      model.streamFailures += 1;
      stopStream();
      renderStatus();
      scheduleStreamRetry();
    };
    ui["live-preview"].src = `/stream.mjpg?t=${Date.now()}`;
  }

  function statusView() {
    const status = model.status;
    if (!model.active) return { title: "页面已暂停预览", detail: "回到此页面后，会自动检查状态并重新连接。", label: "预览已暂停", tone: "waiting" };
    const adjustingView = { title: "正在调整图像参数", detail: "采集链路正在切换，已暂停预览和拍照。恢复新帧后会自动继续，已有照片不会删除。", label: "参数调整中", tone: "waiting" };
    if (model.settingsApplying) return adjustingView;
    if (!model.online || !status || Date.now() - model.lastStatusAt > 3500) {
      return { title: model.lastStatusAt ? "与RK的连接已中断" : "正在连接相机", detail: model.statusError || "正在等待相机服务返回状态，请检查RK与电脑的网络连接。", label: model.lastStatusAt ? "连接中断" : "正在连接", tone: model.lastStatusAt ? "error" : "waiting" };
    }
    if (isReconfiguring()) return adjustingView;
    if (status.state === "error") return { title: "相机采集异常", detail: status.error || "服务暂时无法取得图像，正在等待恢复。", label: "采集异常", tone: "error" };
    if (status.state === "starting") return { title: "相机正在启动", detail: "等待图像稳定后即可预览和拍照。", label: "相机启动中", tone: "waiting" };
    if (!status.fresh) return { title: "画面已过期", detail: "相机暂时没有新图像。已暂停拍照，恢复新帧后会自动继续。", label: "等待新帧", tone: "error" };
    if (!model.streamHealthy) return { title: model.streamFailures ? "预览连接正在恢复" : "正在接收实时画面", detail: "正在连接图像流；连接恢复后会自动显示新画面。", label: "连接预览中", tone: "waiting" };
    return { title: "", detail: "", label: "实时连接", tone: "ready" };
  }

  function renderStatus() {
    const status = model.status;
    const view = statusView();
    const live = view.tone === "ready";
    ui["connection-badge"].dataset.tone = view.tone;
    setText(ui["connection-label"], view.label);
    setText(ui["live-label"], live ? "实时画面" : view.label);
    ui["live-label"].dataset.live = String(live);
    ui["preview-overlay"].hidden = live;
    ui["live-preview"].parentElement.dataset.stale = String(!live);
    setText(ui["overlay-title"], view.title);
    setText(ui["overlay-detail"], view.detail);
    setText(ui["service-state"], isReconfiguring() ? "正在调整参数" : !model.online ? "未连接" : status?.state === "ready" ? "正在运行" : status?.state === "starting" ? "正在启动" : "采集异常");
    ui["service-state"].dataset.tone = model.online && status?.state === "ready" && !isReconfiguring() ? "ready" : "";
    setText(ui["freshness-state"], live ? "持续接收新帧" : view.label);
    ui["freshness-state"].dataset.tone = view.tone;
    const limit = number(status?.capture_limit, 12);
    const count = model.captures.length;
    setText(ui["storage-label"], `${count} / ${limit} 张`);
    const retention = Math.max(1, Math.round(number(status?.retention_s, 1800) / 60));
    setText(ui["retention-note"], `照片临时保存在RK，最多${limit}张，保留约${retention}分钟。主动下载后，才会保存到电脑。`);
    const full = count >= limit;
    ui["capture-button"].disabled = !live || model.capturing || model.deleting || Boolean(model.imageControlsApplying) || full;
    setText(ui["capture-button-label"], model.capturing ? "正在拍照…" : full ? "临时照片已满" : live ? "拍一张照片" : "等待相机");
    setText(ui["capture-hint"], full ? "临时照片数量已达上限，请先下载并删除不再需要的照片。" : "拍照会取得服务接单后收到的新帧，不代表点击瞬间曝光。");
    const error = model.online && status?.error ? status.error : "";
    ui["service-error"].hidden = !error;
    setText(ui["service-error"], error);
    if (status) {
      setText(ui["resolution-label"], `${number(status.width)} × ${number(status.height)}`);
      setText(ui["fps-label"], `预览目标 ${number(status.preview_fps)} 帧/秒 · 采集配置 ${number(status.capture_fps)} 帧/秒`);
      setText(ui["frame-label"], `采集帧 #${number(status.frame_id)}`);
      setText(ui["frame-age"], Number.isFinite(status.frame_age_ms) ? `采集帧龄 ${Math.round(status.frame_age_ms)} ms` : "采集帧龄 —");
      ui["frame-age"].title = "最近一次状态查询返回的帧龄";
      setText(ui["uptime-label"], uptimeLabel(status.uptime_s));
    }
    renderSettings();
    renderQuality();
    renderImageControls();
  }

  async function pollStatus() {
    if (!model.active || model.statusBusy) return;
    model.statusBusy = true;
    const epoch = model.settingsEpoch;
    try {
      const status = await request("/api/status", { timeout: 4000 });
      if (!model.active || epoch !== model.settingsEpoch) return;
      if (!status || !["ready", "starting", "error"].includes(status.state) || typeof status.fresh !== "boolean") throw new Error("相机状态格式无效。");
      model.status = status;
      model.online = true;
      model.statusError = "";
      model.lastStatusAt = Date.now();
      if (!model.settingsApplying) model.settingsAwaitingStatus = false;
      if (status.state === "ready" && status.fresh && !isReconfiguring()) {
        if (!model.streamOpen && streamTimer === null) startStream();
        if (model.settingsMessage === "参数已应用，正在确认新画面。已有照片保持原样。") {
          setSettingsMessage("参数已应用，相机已恢复新帧。已有照片保持原样。");
        }
      } else if (model.streamOpen || streamTimer !== null) stopStream();
      if ((!model.settings || (Number.isFinite(status.settings_version) && status.settings_version !== model.settings.settings_version))
          && Date.now() - model.lastSettingsAttempt >= 5000) void refreshSettings();
      if (Date.now() - model.lastImageControlsAttempt >= 10000) void refreshImageControls();
      if (Date.now() - model.lastListAttempt >= 15000 || (number(status.capture_count) !== model.captures.length && Date.now() - model.lastListAttempt >= 3000)) {
        void refreshCaptures();
      }
    } catch (error) {
      if (error.name !== "AbortError" && model.active) {
        model.online = false;
        model.statusError = error.message;
        stopStream();
      }
    } finally {
      model.statusBusy = false;
      renderStatus();
      if (model.active) statusTimer = setTimeout(pollStatus, 1000);
    }
  }

  async function capturePhoto() {
    if (ui["capture-button"].disabled) return;
    model.capturing = true;
    renderStatus();
    renderSelection();
    try {
      const photo = normalizePhoto(await request("/api/captures", { method: "POST", timeout: 15000, read: false }));
      if (model.disposed) return;
      model.mutationVersion += 1;
      closePhotoDialog();
      clearQuality();
      model.captures = [photo, ...model.captures.filter((item) => item.id !== photo.id)];
      model.selectedId = photo.id;
      renderGallery();
      showNotice(`拍照完成：${photo.id}。照片暂存在RK，可查看或下载原图。`);
    } catch (error) {
      if (error.name !== "AbortError" && !model.disposed) showNotice(`拍照未完成：${error.message}`, true);
    } finally {
      model.capturing = false;
      renderStatus();
      renderSelection();
      model.lastListAttempt = 0;
    }
  }

  async function deleteSelectedPhoto() {
    const photo = selectedPhoto();
    if (!photo || model.deleting || model.capturing) return;
    model.deleting = true;
    renderStatus();
    renderSelection();
    try {
      const result = await request(`/api/captures/${encodeURIComponent(photo.id)}`, { method: "DELETE", read: false });
      if (result.deleted !== true) throw new Error("服务未确认删除结果。");
      if (model.disposed) return;
      model.mutationVersion += 1;
      closePhotoDialog();
      if (model.selectedId === photo.id || model.qualityBusyId === photo.id || model.qualityResult?.capture_id === photo.id) clearQuality();
      model.captures = model.captures.filter((item) => item.id !== photo.id);
      if (model.selectedId === photo.id) model.selectedId = model.captures[0]?.id || null;
      renderGallery();
      showNotice(`已删除照片 ${photo.id}。`);
    } catch (error) {
      if (error.name !== "AbortError" && !model.disposed) showNotice(`删除未完成：${error.message}`, true);
    } finally {
      model.deleting = false;
      model.lastListAttempt = 0;
      renderStatus();
      renderSelection();
    }
  }

  function activate() {
    if (model.disposed || document.hidden || model.active) return;
    model.active = true;
    model.online = false;
    model.statusError = "";
    model.lastListAttempt = 0;
    model.lastSettingsAttempt = 0;
    model.lastImageControlsAttempt = 0;
    void pollStatus();
    void refreshCaptures();
    void refreshSettings();
    void refreshImageControls();
    presentationTimer = setInterval(() => {
      if (model.streamOpen && !model.streamHealthy && ui["live-preview"].naturalWidth > 0) {
        model.streamHealthy = true;
        model.streamFailures = 0;
      }
      if (model.streamOpen && !model.streamHealthy && Date.now() - model.streamStartedAt > 8000) {
        model.streamFailures += 1;
        stopStream();
        scheduleStreamRetry();
      }
      renderStatus();
    }, 500);
    renderStatus();
  }

  function deactivate() {
    model.active = false;
    clearTimeout(statusTimer);
    clearInterval(presentationTimer);
    statusTimer = null;
    presentationTimer = null;
    stopStream();
    for (const controller of reads) controller.abort();
    renderStatus();
  }

  ui["capture-button"].addEventListener("click", capturePhoto);
  ui["check-quality"].addEventListener("click", checkPhotoQuality);
  ui["settings-form"].addEventListener("submit", applySettings);
  ui["exposure-form"].addEventListener("submit", (event) => { void applyImageControls(event, "exposure"); });
  ui["white-balance-form"].addEventListener("submit", (event) => { void applyImageControls(event, "white_balance"); });
  for (const control of [ui["exposure-mode"], ui["exposure-time"], ui["exposure-gain"]]) {
    control.addEventListener("input", () => {
      model.exposureMessage = "";
      model.exposureMessageError = false;
      updateImageControlsDirty();
    });
  }
  for (const control of [ui["white-balance-mode"], ...wbGainKeys.map((key) => ui[`wb-${key}`])]) {
    control.addEventListener("input", () => {
      model.whiteBalanceMessage = "";
      model.whiteBalanceMessageError = false;
      updateImageControlsDirty();
    });
  }
  for (const control of [ui["setting-resolution"], ui["setting-fps"], ui["setting-quality"]]) {
    control.addEventListener("input", () => {
      model.settingsMessage = "";
      model.settingsMessageError = false;
      updateSettingsDirty();
    });
  }
  ui["delete-photo"].addEventListener("click", deleteSelectedPhoto);
  ui["dismiss-notice"].addEventListener("click", () => { clearTimeout(noticeTimer); ui.notice.hidden = true; });
  ui["photo-gallery"].addEventListener("click", (event) => {
    const button = event.target.closest("button[data-photo-id]");
    if (!button || !ui["photo-gallery"].contains(button)) return;
    if (model.selectedId !== button.dataset.photoId) clearQuality();
    model.selectedId = button.dataset.photoId;
    updateGallerySelection();
  });
  ui["open-photo"].addEventListener("click", () => {
    const photo = selectedPhoto();
    if (!photo) return;
    setText(ui["dialog-title"], photo.id);
    setText(ui["dialog-caption"], `${photo.width} × ${photo.height} · 板端接收 ${dateLabel(photo.received_at)}`);
    ui["dialog-image"].src = photo.url;
    ui["dialog-download"].href = photo.download_url;
    ui["dialog-download"].download = `${photo.id}.jpg`;
    if (!ui["photo-dialog"].open) ui["photo-dialog"].showModal();
  });
  ui["close-dialog"].addEventListener("click", closePhotoDialog);
  ui["photo-dialog"].addEventListener("close", () => { ui["dialog-image"].removeAttribute("src"); ui["dialog-download"].removeAttribute("href"); });
  ui["photo-dialog"].addEventListener("click", (event) => { if (event.target === ui["photo-dialog"]) closePhotoDialog(); });
  ui["selected-image"].addEventListener("error", () => {
    if (model.selectedId && model.active) { showNotice("照片可能已过期或被删除，正在更新列表。", true); model.lastListAttempt = 0; void refreshCaptures(); }
  });
  document.addEventListener("visibilitychange", () => { if (document.hidden) deactivate(); else activate(); });
  window.addEventListener("pagehide", deactivate);
  window.addEventListener("pageshow", () => { model.disposed = false; activate(); });
  window.addEventListener("beforeunload", () => {
    model.disposed = true;
    deactivate();
    clearTimeout(noticeTimer);
    for (const controller of requests) controller.abort();
    closePhotoDialog();
  });
  activate();
})();
