# Settings reference

The machine-readable reference is [`shared/settings-schema.json`](../shared/settings-schema.json). It currently exposes the Phase 2 preview-safe FreeType controls: hinting, anti-aliasing mode, normal and bold weight, italic slant, kerning, gamma, contrast, and render weight.

Every entry records its INI section/key, value type, validated range, default, preview behavior, apply mode, and legacy `IControlCenter` ordinal. New settings must be added to the JSON first and regenerated with `pnpm --dir control-center generate:settings`; hand-written ordinal copies are not accepted by CI.
