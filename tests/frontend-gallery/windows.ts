export const galleryViews = [
  {
    id: "overview",
    title: { ko: "개요", en: "Overview", "zh-CN": "概览", "zh-TW": "總覽", ja: "概要", fr: "Vue d’ensemble", de: "Übersicht", es: "Resumen", pt: "Visão geral", ar: "نظرة عامة" },
  },
  {
    id: "files",
    title: { ko: "설정 파일", en: "Settings files", "zh-CN": "设置文件", "zh-TW": "設定檔案", ja: "設定ファイル", fr: "Fichiers de réglages", de: "Einstellungsdateien", es: "Archivos de configuración", pt: "Ficheiros de definições", ar: "ملفات الإعدادات" },
  },
  {
    id: "profiles",
    title: { ko: "고급 조정", en: "Advanced tuning", "zh-CN": "高级调校", "zh-TW": "進階調校", ja: "詳細調整", fr: "Réglages avancés", de: "Erweiterte Anpassung", es: "Ajuste avanzado", pt: "Ajuste avançado", ar: "الضبط المتقدم" },
  },
  {
    id: "execution",
    title: { ko: "서비스", en: "Service", "zh-CN": "服务", "zh-TW": "服務", ja: "サービス", fr: "Service", de: "Dienst", es: "Servicio", pt: "Serviço", ar: "الخدمة" },
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
