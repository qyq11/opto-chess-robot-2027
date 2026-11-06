(() => {
  "use strict";
  const names = ["state", "link-label", "query", "confirm", "speed", "speed-number", "speed-note",
    "duration", "duration-number", "draft-label", "summary", "forward", "backward", "left", "right", "stop",
    "running", "running-label", "countdown", "progress", "apply-speed", "message", "feedback",
    "feedback-status", "log", "m1", "m2", "m3", "m4", "wheel-1", "wheel-2", "wheel-3", "wheel-4",
    "distance", "distance-number", "distance-confirm", "distance-result", "distance-target", "distance-estimate", "distance-phase", "running-note"];
  const ui = Object.fromEntries(names.map((name) => [name, document.getElementById(`motion-${name}`)]));
  const movements = { forward: "前进", backward: "后退", left: "左移", right: "右移" };
  let state = null;
  let limits = null;
  let lastRead = 0;
  let pending = 0;
  let epoch = 0;
  let reading = false;
  let hidden = document.hidden;
  let draftDirty = false;
  const phases = { waiting_encoder: "等待静止计数基准", accelerating: "缓慢起步", cruising: "正在前进", decelerating: "接近目标，正在减速", stopped: "已发送停止" };
  const stops = { target_reached: "轮子估计达到目标，已发送停止", manual: "手动停止", time_limit: "达到时间上限，未确认到达目标", encoder_timeout: "计数反馈缺失，已发送停止", encoder_jump: "计数突变，已发送停止", wrong_direction: "方向不符，已发送停止", no_progress: "计数无进展，已发送停止", speed_feedback_timeout: "轮速反馈中断，已发送停止", serial_error: "串口异常，停止结果需现场核对" };

  function distanceDraft() {
    const cm = ui["distance-number"].valueAsNumber, range = limits?.distance_mm;
    return range && Number.isInteger(cm) && cm * 10 >= range.min && cm * 10 <= range.max &&
      (cm * 10 - range.min) % range.step === 0 ? cm * 10 : null;
  }

  function setText(element, value) {
    if (element.textContent !== String(value)) element.textContent = String(value);
  }

  function message(text, error = false) {
    setText(ui.message, text);
    ui.message.dataset.tone = error ? "error" : "normal";
  }

  function draft() {
    const speed = ui["speed-number"].valueAsNumber;
    const duration = ui["duration-number"].valueAsNumber;
    if (!limits || !Number.isInteger(speed) || !Number.isInteger(duration)) return null;
    if (speed * 10 < limits.speed_mm_s.min || speed * 10 > limits.speed_mm_s.max ||
        (speed * 10 - limits.speed_mm_s.min) % limits.speed_mm_s.step ||
        duration * 1000 < limits.duration_ms.min || duration * 1000 > limits.duration_ms.max ||
        (duration * 1000 - limits.duration_ms.min) % limits.duration_ms.step) return null;
    return { speed_mm_s: speed * 10, duration_ms: duration * 1000 };
  }

  function paintRange(input) {
    const min = Number(input.min), max = Number(input.max);
    const percent = (Number(input.value) - min) * 100 / (max - min);
    input.style.setProperty("--range-fill", `${Math.max(0, Math.min(100, percent))}%`);
  }

  function accept(value) {
    if (typeof value.opened !== "boolean" || !Array.isArray(value.events) || !value.limits)
      throw new Error("运动接口返回格式不正确，请刷新页面。");
    for (const name of ["speed_mm_s", "duration_ms", "distance_mm"]) {
      const range = value.limits[name];
      if (!range || ![range.min, range.max, range.step, range.default].every(Number.isInteger) ||
          range.min >= range.max || range.step <= 0 || range.default < range.min || range.default > range.max)
        throw new Error("板端没有提供有效的参数范围。");
    }
    if (!limits) {
      limits = value.limits;
      for (const [name, key, scale] of [["speed", "speed_mm_s", 10], ["duration", "duration_ms", 1000]]) {
        for (const input of [ui[name], ui[`${name}-number`]]) {
          input.min = String(limits[key].min / scale);
          input.max = String(limits[key].max / scale);
          input.step = String(limits[key].step / scale);
          if (!draftDirty) input.value = String(limits[key].default / scale);
        }
        paintRange(ui[name]);
      }
      const range = limits.distance_mm;
      ui["distance-number"].min = String(range.min / 10);
      ui["distance-number"].max = String(range.max / 10);
      ui["distance-number"].step = String(range.step / 10);
      ui["distance-number"].value = String(range.default / 10);
    }
    state = value;
    lastRead = Date.now();
  }

  function render() {
    const fresh = !hidden && Date.now() - lastRead < 1800;
    const available = fresh && state?.opened && !state.fault;
    const busy = pending > 0 || state?.moving || state?.querying;
    const selected = draft();
    ui.query.disabled = !available || busy;
    for (const action of Object.keys(movements))
      ui[action].disabled = !available || !state.ready || busy || !ui.confirm.checked || !selected;
    ui.distance.disabled = !available || !state?.ready || busy || !ui["distance-confirm"].checked || distanceDraft() === null;
    ui["distance-number"].disabled = Boolean(busy);
    ui["distance-confirm"].disabled = Boolean(busy);
    for (let i = 1; i <= 4; i++) ui[`m${i}`].disabled = busy;
    for (const name of ["speed", "speed-number", "duration", "duration-number"])
      ui[name].disabled = pending > 0 || Boolean(state?.querying);
    ui["apply-speed"].disabled = !available || !state?.moving || pending > 0 || !ui.confirm.checked ||
      !selected || selected.speed_mm_s === state.speed_mm_s || state.mode === "distance";
    ui.state.dataset.tone = !fresh || state?.fault || state?.error ? "error" : state?.moving ? "moving" : state?.ready ? "ready" : "waiting";
    setText(ui.state, !fresh ? "连接待确认" : state?.fault ? "串口异常" : !state?.opened ? "串口不可用"
      : state.querying ? "读取中" : state.moving ? "正在运行" : state.ready ? "就绪" : "等待读取");
    setText(ui["link-label"], !fresh ? "正在确认板端连接" : state?.ready ? "已收到驱动真实回复"
      : state?.opened ? "串口已打开，等待读取驱动" : "检查串口服务与接线");
    setText(ui["draft-label"], state?.moving ? "下一次参数" : "本次设置");
    setText(ui.summary, selected ? `${selected.speed_mm_s / 10}厘米/秒，运行${selected.duration_ms / 1000}秒` : "请填写范围内的整数");
    const unchecked = Boolean(selected && selected.speed_mm_s > 60);
    ui["speed-note"].dataset.unchecked = String(unchecked);
    setText(ui["speed-note"], unchecked ? "较高档位，请先架空核对" : "先从低档位核对方向");
    ui.running.hidden = !state?.moving;
    if (state?.moving) {
      const remaining = Math.max(0, state.remaining_ms - (Date.now() - lastRead));
      const byDistance = state.mode === "distance";
      setText(ui["running-label"], byDistance ? `按距离前进 · 目标${state.distance_mm / 10}厘米`
        : `${movements[state.action] || "运行中"} · 当前目标${state.speed_mm_s / 10}厘米/秒`);
      setText(ui.countdown, !fresh ? "状态待确认" : byDistance ? state.remaining_mm === null ? "等待计数" : `还差${(state.remaining_mm / 10).toFixed(1)}厘米` : `剩余${(remaining / 1000).toFixed(1)}秒`);
      const progress = byDistance ? (state.estimated_mm ?? 0) / state.distance_mm : 1 - remaining / state.duration_ms;
      ui.progress.value = fresh ? Math.max(0, Math.min(100, 100 * progress)) : 0;
      ui["apply-speed"].hidden = byDistance;
      setText(ui["running-note"], byDistance ? `轮速由RK控制，最多运行15秒；当前上限还剩${(remaining / 1000).toFixed(1)}秒。` : "只调整速度，停止时刻保持不变。");
    }
    ui["distance-result"].hidden = state?.mode !== "distance";
    if (state?.mode === "distance") {
      setText(ui["distance-target"], `${state.distance_mm / 10}厘米`);
      setText(ui["distance-estimate"], state.estimated_mm === null ? "等待计数基准" : `${(state.estimated_mm / 10).toFixed(1)}厘米`);
      setText(ui["distance-phase"], !fresh ? "状态待确认" : state.moving ? phases[state.distance_phase] || "运行中" : stops[state.stop_reason] || "已发送停止，待核对实物");
    }
    const hasFeedback = Array.isArray(state?.actual_mm_s) && state.actual_mm_s.length === 4;
    for (let i = 1; i <= 4; i++) {
      let value = hasFeedback ? state.actual_mm_s[i - 1] / 10 : NaN;
      if (Math.abs(value) < 0.05) value = 0;
      setText(ui[`wheel-${i}`], Number.isFinite(value) ? value.toFixed(1) : "—");
    }
    setText(ui["feedback-status"], hasFeedback ? state.moving && fresh ? "实时回报 · 厘米/秒" : "最近一次回报" : "等待回报");
    if (state?.events) setText(ui.log, state.events.join("\n\n") || "尚无收发");
    setText(ui.feedback, hasFeedback ? `收到${state.feedback_samples}组轮速；本次各轮绝对值峰值：${state.peak_mm_s.map((value) => (value / 10).toFixed(1)).join(" / ")}厘米/秒（M1至M4）。`
      : "尚未收到有效轮速回包。开始试转后会采集实际反馈。");
  }

  async function api(action, fields = {}) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 2500);
    try {
      const response = await fetch("/api/motion", {
        method: action ? "POST" : "GET", cache: "no-store", credentials: "same-origin",
        headers: action ? { "Content-Type": "application/x-www-form-urlencoded", "X-Motion-Control": "1" } : {},
        body: action ? new URLSearchParams({ action, ...fields }) : undefined,
        signal: controller.signal,
      });
      const value = await response.json();
      if (!response.ok) throw new Error(value.error || `操作失败（${response.status}）`);
      return value;
    } finally { clearTimeout(timeout); }
  }

  async function refresh() {
    if (hidden || reading || pending) return;
    reading = true;
    const startedEpoch = epoch;
    try {
      const value = await api();
      if (startedEpoch !== epoch) return;
      const previous = state;
      accept(value);
      if (state.error) message(state.error, true);
      else if (previous?.moving && !state.moving) message(state.mode === "distance"
        ? `${stops[state.stop_reason] || "已发送停止"}。请核对停车，并用尺子测量车身位移。`
        : "已发送停止输出，请结合轮速回报核对轮子是否停下。");
      else if (previous?.querying && state.ready) message("驱动已回复。确认现场后，可以开始试转。");
    } catch (error) {
      if (startedEpoch === epoch) { lastRead = 0; message("无法确认板端状态，请检查连接和轮子是否停止。", true); }
    } finally { reading = false; render(); }
  }

  async function command(action) {
    const selected = draft();
    const movement = Object.hasOwn(movements, action);
    const byDistance = action === "distance";
    if (action !== "stop" && (pending || state?.querying)) return;
    if ((movement || action === "speed") &&
        (!ui.confirm.checked || !state?.ready || hidden || Date.now() - lastRead >= 1800 || !selected)) return;
    if (movement && state?.moving) return;
    if (byDistance && (!ui["distance-confirm"].checked || !state?.ready || hidden || state.moving ||
        Date.now() - lastRead >= 1800 || distanceDraft() === null)) return;
    if (action === "speed" && !state?.moving) return;
    const currentEpoch = ++epoch;
    ++pending;
    const fields = {};
    if (byDistance) {
      fields.generation = String(state.generation);
      fields.distance_mm = String(distanceDraft());
      for (let i = 1; i <= 4; i++) fields[`m${i}`] = ui[`m${i}`].value;
    }
    if (movement || action === "speed") {
      fields.generation = String(state.generation);
      fields.speed_mm_s = String(selected.speed_mm_s);
      if (action !== "speed") {
        fields.duration_ms = String(selected.duration_ms);
        for (let i = 1; i <= 4; i++) fields[`m${i}`] = ui[`m${i}`].value;
      }
    }
    if (action === "query") ui.confirm.checked = false;
    message(action === "stop" ? "正在发送停止输出…" : action === "query" ? "正在读取驱动…"
      : action === "speed" ? "正在调整本次目标速度…" : "正在提交本次速度和时间…");
    render();
    try {
      const value = await api(action, fields);
      if (currentEpoch !== epoch) return;
      accept(value);
      message(action === "stop" ? "已发送停止输出，请核对轮子是否停下。"
        : action === "query" ? "查询已发送，等待驱动真实回复…"
        : byDistance ? `本次前进${state.distance_mm / 10}厘米，先等待计数基准，再缓慢起步。请用尺子核对实际位移。`
        : action === "speed" ? `当前目标改为${state.speed_mm_s / 10}厘米/秒，按原定时刻停止。`
        : `本次${movements[action]}，目标${state.speed_mm_s / 10}厘米/秒，运行${state.duration_ms / 1000}秒，起步会逐渐加速。`);
    } catch (error) {
      if (currentEpoch === epoch) {
        lastRead = 0;
        message(`操作结果待确认：${error.name === "AbortError" ? "请求超时" : error.message}。请观察轮子，必要时现场停机。`, true);
      }
    } finally { --pending; render(); void refresh(); }
  }

  for (const name of ["speed", "duration"]) {
    ui[name].addEventListener("input", () => {
      draftDirty = true;
      ui[`${name}-number`].value = ui[name].value;
      paintRange(ui[name]);
      render();
    });
    ui[`${name}-number`].addEventListener("input", () => {
      draftDirty = true;
      if (ui[`${name}-number`].checkValidity() && ui[`${name}-number`].value !== "") {
        ui[name].value = ui[`${name}-number`].value;
        paintRange(ui[name]);
      }
      render();
    });
    paintRange(ui[name]);
  }
  ui.query.addEventListener("click", () => { void command("query"); });
  ui.distance.addEventListener("click", () => { void command("distance"); });
  ui["distance-number"].addEventListener("input", render);
  ui["distance-confirm"].addEventListener("change", render);
  for (const action of Object.keys(movements))
    ui[action].addEventListener("click", () => { void command(action); });
  ui.stop.addEventListener("click", () => { void command("stop"); });
  ui["apply-speed"].addEventListener("click", () => { void command("speed"); });
  ui.confirm.addEventListener("change", render);
  for (let i = 1; i <= 4; i++) ui[`m${i}`].addEventListener("change", () => {
    ui.confirm.checked = false;
    ui["distance-confirm"].checked = false;
    message("方向已调整，请重新确认现场后再试转。");
    render();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !event.repeat && (state?.moving || pending)) void command("stop");
  });
  document.addEventListener("visibilitychange", () => {
    hidden = document.hidden;
    ui.confirm.checked = false;
    ui["distance-confirm"].checked = false;
    if (hidden && (state?.moving || pending)) void command("stop");
    if (!hidden) { lastRead = 0; void refresh(); }
    render();
  });
  window.addEventListener("pagehide", () => {
    if (state?.moving || pending) void fetch("/api/motion", { method: "POST", keepalive: true,
      headers: { "Content-Type": "application/x-www-form-urlencoded", "X-Motion-Control": "1" },
      body: "action=stop" }).catch(() => {});
  });
  setInterval(() => { if (!hidden) render(); }, 100);
  setInterval(() => { void refresh(); }, 400);
  render();
  void refresh();
})();
