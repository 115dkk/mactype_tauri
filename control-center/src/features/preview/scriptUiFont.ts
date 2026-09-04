import type { Locale } from "../../i18n/i18n";

/* The system UI face beside Segoe UI for the reader's script, named the way
   GDI resolves it. */
export function scriptUiFont(locale: Locale): string | null {
  switch (locale) {
    case "ko": return "Malgun Gothic";
    case "ja": return "Yu Gothic UI";
    case "zh-CN": return "Microsoft YaHei UI";
    case "zh-TW": return "Microsoft JhengHei UI";
    default: return null;
  }
}
