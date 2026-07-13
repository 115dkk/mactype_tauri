export const galleryViews = [
  {
    id: "overview",
    title: { ko: "개요", en: "Overview", "zh-CN": "概览", "zh-TW": "總覽", ja: "概要", fr: "Vue d’ensemble", de: "Übersicht", es: "Resumen", pt: "Visão geral", ar: "نظرة عامة" },
  },
  {
    id: "profiles",
    title: { ko: "프로필", en: "Profiles", "zh-CN": "配置文件", "zh-TW": "設定檔", ja: "プロファイル", fr: "Profils", de: "Profile", es: "Perfiles", pt: "Perfis", ar: "ملفات التعريف" },
  },
  {
    id: "execution",
    title: { ko: "실행", en: "Execution", "zh-CN": "运行", "zh-TW": "執行", ja: "実行", fr: "Exécution", de: "Ausführung", es: "Ejecución", pt: "Execução", ar: "التشغيل" },
  },
  {
    id: "diagnostics",
    title: { ko: "진단", en: "Diagnostics", "zh-CN": "诊断", "zh-TW": "診斷", ja: "診断", fr: "Diagnostic", de: "Diagnose", es: "Diagnóstico", pt: "Diagnóstico", ar: "التشخيص" },
  },
] as const;

export const galleryLocales = [
  { id: "ko", direction: "ltr", script: /[가-힣]/u },
  { id: "en", direction: "ltr", script: /[A-Za-z]/u },
  { id: "zh-CN", direction: "ltr", script: /[一-龯]/u },
  { id: "zh-TW", direction: "ltr", script: /[一-龯]/u },
  { id: "ja", direction: "ltr", script: /[ぁ-んァ-ン]/u },
  { id: "fr", direction: "ltr", script: /[A-Za-zÀ-ÿ]/u },
  { id: "de", direction: "ltr", script: /[A-Za-zÄÖÜäöüß]/u },
  { id: "es", direction: "ltr", script: /[A-Za-zÁÉÍÓÚÜÑáéíóúüñ]/u },
  { id: "pt", direction: "ltr", script: /[A-Za-zÀ-ÿ]/u },
  { id: "ar", direction: "rtl", script: /[ء-ي]/u },
] as const;
