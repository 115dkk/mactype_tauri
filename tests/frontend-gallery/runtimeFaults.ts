import type { Page } from "@playwright/test";
import { fallbackStatus, type ViewId } from "../../control-center/src/app/model";
import { galleryExecutionStatus } from "../../control-center/src/app/runtimeAdapters/browserGalleryExecution";
import { settingsSchema } from "../../control-center/src/generated/settings";

interface RuntimeFaultOptions {
  reject?: string[];
  rejectEvent?: string;
  holdLaunch?: boolean;
  failRender?: boolean;
  holdEvent?: string;
  view?: ViewId;
  ciSmoke?: boolean;
  trayStart?: boolean;
  previewStudioSmoke?: boolean;
}

export interface RuntimeFaultState {
  calls: Array<{ command: string; args: Record<string, unknown> }>;
  unhandled: string[];
  unexpected: string[];
  settleLaunch: (reject: boolean) => void;
  releaseListeners: () => void;
}

declare global {
  interface Window {
    galleryFaults: RuntimeFaultState;
  }
}

export async function installRuntimeFaults(page: Page, options: RuntimeFaultOptions = {}) {
  await page.addInitScript(({ options, status, execution, values }) => {
    const launch = {
      view: options.view ?? "overview",
      ciSmoke: options.ciSmoke ?? false,
      trayStart: options.trayStart ?? false,
      previewStudioSmoke: options.previewStudioSmoke ?? false,
    };
    const pendingLaunch: Array<(reject: boolean) => void> = [];
    const pendingListeners: Array<() => void> = [];
    const faults: RuntimeFaultState = {
      calls: [], unhandled: [], unexpected: [],
      settleLaunch: (reject) => pendingLaunch.splice(0).forEach((settle) => settle(reject)),
      releaseListeners: () => pendingListeners.splice(0).forEach((release) => release()),
    };
    window.galleryFaults = faults;
    window.addEventListener("unhandledrejection", (event) => faults.unhandled.push(String(event.reason)));
    const profile = {
      path: "C:\\Gallery\\Default.ini", displayPath: "ini\\Default.ini", location: "installation", canSave: true,
      encoding: "utf-8", bom: "none", lineEnding: "cr-lf", originalHash: "gallery",
      values, savedValues: values, dirtyKeys: [], canUndo: false, canRedo: false, individuals: [],
      lists: { excludeFonts: [], includeFonts: [], excludeModules: [], includeModules: [], unloadDlls: [], excludeSubstitutionModules: [], unityIncludeGames: [], unityExcludeGames: [] },
      advanced: { shadow: null, lcdFilterWeight: null, pixelLayout: null, fontSubstitutes: [], fontSubstituteBoldPairs: [] },
    };
    if (options.failRender) Object.defineProperty(execution, "systemService", {
      get: () => { throw new Error("fixture:render"); },
    });
    let callbackId = 0;
    let eventId = 0;
    let requestId = 0;
    Object.defineProperty(window, "__TAURI_INTERNALS__", { value: {
      metadata: { currentWindow: { label: "main" }, currentWebview: { label: "main" } },
      transformCallback: () => ++callbackId,
      unregisterCallback: () => undefined,
      convertFileSrc: (path: string) => path,
      invoke: async (command: string, args: Record<string, unknown> = {}) => {
        faults.calls.push({ command, args });
        if (options.reject?.includes(command)) throw `fixture:${command}`;
        if (command === "plugin:event|listen") {
          if (args.event === options.rejectEvent) throw `fixture:listen:${args.event}`;
          const id = ++eventId;
          if (args.event === options.holdEvent) return new Promise((resolve) => pendingListeners.push(() => resolve(id)));
          return id;
        }
        switch (command) {
          case "launch_context":
            if (options.holdLaunch) return new Promise((resolve, reject) => pendingLaunch.push((fail) => fail ? reject("fixture:late-launch") : resolve(launch)));
            return launch;
          case "scan_installation": return status;
          case "execution_status": return execution;
          case "current_profile":
          case "open_default_profile":
          case "open_profile": return profile;
          case "discover_legacy_profile": return null;
          case "list_profiles": return [{ name: "Default", path: profile.path, displayPath: profile.displayPath }];
          case "installed_font_families": return ["Segoe UI"];
          case "recent_activity":
          case "list_events":
          case "diagnostic_recent_logs":
          case "preview_diagnostics":
          case "preview_font_substitutes":
          case "list_manual_launch_candidates": return [];
          case "event_log_summary": return { total: 0, warnings: 0, errors: 0, newestTs: null, sources: [] };
          case "diagnostic_report": return "Gallery diagnostics";
          case "render_profile_preview": {
            const sample = args.sample as { widthPx: number; heightPx: number; dpi: number };
            return {
              requestId: ++requestId,
              imagePath: `data:image/svg+xml,${encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" width="${sample.widthPx}" height="${sample.heightPx}"/>`)}`,
              width: sample.widthPx, height: sample.heightPx, dpi: sample.dpi, elapsedMs: 1, coreVersion: 1,
            };
          }
          case "set_application_locale":
          case "frontend_ready":
          case "frontend_failed":
          case "preview_studio_ready":
          case "open_preview_studio":
          case "ci_verify_tray_mode":
          case "ci_verify_profile_workflow":
          case "ci_verify_injection_workflow":
          case "ci_force_preview_crash":
          case "plugin:event|unlisten":
          case "plugin:event|emit": return;
          default:
            faults.unexpected.push(command);
            throw new Error(`Unexpected fixture command: ${command}`);
        }
      },
    } });
    Object.defineProperty(window, "__TAURI_EVENT_PLUGIN_INTERNALS__", { value: { unregisterListener: () => undefined } });
  }, {
    options,
    status: fallbackStatus,
    execution: galleryExecutionStatus(new URLSearchParams()),
    values: Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])),
  });
}

export async function runtimeFaultSnapshot(page: Page) {
  return page.evaluate(() => ({
    calls: window.galleryFaults.calls,
    unhandled: window.galleryFaults.unhandled,
    unexpected: window.galleryFaults.unexpected,
  }));
}
