import type { StatusTone } from "../../components/StatusDot";

export function serviceTone(tone: "normal" | "neutral" | "attention" | "critical"): StatusTone {
  return tone === "normal" ? "ok" : tone === "attention" ? "warn" : tone === "critical" ? "bad" : "neutral";
}
