(async () => {
const ffi = globalThis.KiraBrowserFFI;
const wasmBytes = await fetch("./kira-app.wasm").then((response) => response.arrayBuffer());
const wasm = await WebAssembly.instantiate(wasmBytes, {});
const exports = wasm.instance.exports;
const wasmModuleLoaded = exports.kira_wasm_module_loaded();
const runtimeStarted = exports.kira_runtime_started();
const appEntrypointInvoked = exports.kira_app_entrypoint_invoked();
const uiFoundationStarted = exports.kira_ui_foundation_app_started();
const uiTreeBuilt = exports.kira_ui_tree_built();
const uiRetainedTreeReady = exports.kira_ui_retained_tree_ready();
const uiLayoutNonEmpty = exports.kira_ui_layout_non_empty();
const uiDrawCommandsSubmitted = exports.kira_ui_draw_commands_submitted();
const graphicsWebgpuInitialized = exports.kira_graphics_webgpu_initialized();
const appStarted = exports.kira_app_start();
const retainedTreeInitialized = exports.kira_retained_tree_initialized();
const layoutRan = exports.kira_layout_ran();
const renderCommandsGenerated = exports.kira_render_commands_generated();
globalThis.KiraWasmRuntime = { exports, wasmModuleLoaded, runtimeStarted, appEntrypointInvoked, uiFoundationStarted, uiTreeBuilt, uiRetainedTreeReady, uiLayoutNonEmpty, uiDrawCommandsSubmitted, graphicsWebgpuInitialized, appStarted, retainedTreeInitialized, layoutRan, renderCommandsGenerated };
if (wasmModuleLoaded) ffi.consoleLog("KIRA_WASM_MODULE_LOADED");
if (runtimeStarted) ffi.consoleLog("KIRA_RUNTIME_STARTED");
if (appEntrypointInvoked) ffi.consoleLog("KIRA_APP_ENTRYPOINT_INVOKED");
if (uiFoundationStarted) ffi.consoleLog("KIRA_UI_FOUNDATION_APP_STARTED");
if (uiTreeBuilt) ffi.consoleLog("KIRA_UI_TREE_BUILT");
if (uiRetainedTreeReady) ffi.consoleLog("KIRA_UI_RETAINED_TREE_READY");
if (uiLayoutNonEmpty) ffi.consoleLog("KIRA_UI_LAYOUT_NON_EMPTY");
if (uiDrawCommandsSubmitted) ffi.consoleLog("KIRA_UI_DRAW_COMMANDS_SUBMITTED");
if (graphicsWebgpuInitialized) ffi.consoleLog("KIRA_GRAPHICS_WEBGPU_INITIALIZED");
ffi.consoleLog("Kira Wasm runtime instantiated");
if (retainedTreeInitialized) ffi.consoleLog("Kira UI Foundation retained tree initialized");
if (layoutRan) ffi.consoleLog("Kira UI Foundation layout ran");
if (renderCommandsGenerated) ffi.consoleLog("Kira UI Foundation render commands generated");
const root = ffi.documentBody();
const title = ffi.createElement("h1");
ffi.setText(title, "Kira WebGPU surface");
ffi.appendChild(root, title);
const canvas = ffi.createCanvas();
ffi.setAttribute(canvas, "width", "640");
ffi.setAttribute(canvas, "height", "360");
ffi.setStyle(canvas, "border", "1px solid #222");
ffi.appendChild(root, canvas);
const status = ffi.createElement("p");
ffi.setText(status, "Detecting WebGPU");
ffi.appendChild(root, status);
try {
  const info = await ffi.detectWebGPU();
  if (!info.available || !info.adapter) {
    ffi.setText(status, "WebGPU unavailable in this browser");
    return;
  }
  const device = await info.adapter.requestDevice();
  const context = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: "opaque" });
  const shader = device.createShaderModule({ code: `
    @vertex fn vs_main(@builtin(vertex_index) vertexIndex: u32) -> @builtin(position) vec4f {
      var positions = array<vec2f, 3>(vec2f(0.0, 0.7), vec2f(-0.7, -0.7), vec2f(0.7, -0.7));
      let p = positions[vertexIndex];
      return vec4f(p, 0.0, 1.0);
    }
    @fragment fn fs_main() -> @location(0) vec4f {
      return vec4f(0.16, 0.62, 0.52, 1.0);
    }
  ` });
  const pipeline = device.createRenderPipeline({
    layout: "auto",
    vertex: { module: shader, entryPoint: "vs_main" },
    fragment: { module: shader, entryPoint: "fs_main", targets: [{ format }] },
    primitive: { topology: "triangle-list" },
  });
  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({
    colorAttachments: [{ view: context.getCurrentTexture().createView(), clearValue: { r: 0.04, g: 0.05, b: 0.07, a: 1.0 }, loadOp: "clear", storeOp: "store" }],
  });
  pass.setPipeline(pipeline);
  pass.draw(3);
  pass.end();
  device.queue.submit([encoder.finish()]);
  globalThis.KiraWebGpuSmoke = { device: true, context: true, pipeline: true, frame: true };
  ffi.setText(status, "WebGPU frame rendered");
  if (exports.kira_webgpu_pipeline_created()) ffi.consoleLog("KIRA_WEBGPU_PIPELINE_CREATED");
  if (exports.kira_webgpu_frame_rendered()) ffi.consoleLog("KIRA_WEBGPU_FRAME_RENDERED");
  ffi.consoleLog("Kira WebGPU capability detection completed");
  ffi.consoleLog("Kira WebGPU pipeline created");
  ffi.consoleLog("Kira WebGPU frame rendered");
} catch (error) {
  ffi.setText(status, "WebGPU detection failed");
  throw error;
}
})();
