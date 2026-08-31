#![forbid(unsafe_code)]

use mactype_service_contract::{
    GenerationPointer, MachinePaths, PrivateFreeTypePolicy, RendererRuntimeBinding,
    RuntimeGenerationPointer, StructuredServiceError, UnityFontHookPolicy,
};

use crate::profile_runtime::{
    ensure_activation_state_is_stable, read_active_profile_pointer, read_active_runtime_pointer,
    ProtectedProfileSnapshot,
};
use crate::runtime_assets::ProtectedRuntimeAssets;

#[derive(Debug)]
pub struct ProtectedRendererRuntime {
    assets: ProtectedRuntimeAssets,
    binding: RendererRuntimeBinding,
    unity_font_hook: UnityFontHookPolicy,
    private_freetype: PrivateFreeTypePolicy,
}

impl ProtectedRendererRuntime {
    pub fn load(paths: MachinePaths) -> Result<Self, StructuredServiceError> {
        Self::load_with_stability_hook(paths, || {})
    }

    #[cfg(feature = "ci-test-adapter")]
    pub fn load_with_pointer_stability_hook_for_ci<F>(
        paths: MachinePaths,
        hook: F,
    ) -> Result<Self, StructuredServiceError>
    where
        F: FnOnce(),
    {
        Self::load_with_stability_hook(paths, hook)
    }

    fn load_with_stability_hook<F>(
        paths: MachinePaths,
        hook: F,
    ) -> Result<Self, StructuredServiceError>
    where
        F: FnOnce(),
    {
        let before = ActivePointerPair::capture(&paths)?;
        let assets = ProtectedRuntimeAssets::load(&paths, &before.runtime)?;
        let profile = ProtectedProfileSnapshot::load(&paths, &before.profile, assets.root())?;

        hook();

        let after = ActivePointerPair::capture(&paths)?;
        if before.profile_bytes != after.profile_bytes
            || before.runtime_bytes != after.runtime_bytes
        {
            return Err(service_error(
                "renderer-runtime-binding-changed",
                "the active profile or runtime pointer changed while the renderer binding was verified",
            ));
        }
        let verified_assets = ProtectedRuntimeAssets::load(&paths, &after.runtime)?;
        let verified_profile =
            ProtectedProfileSnapshot::load(&paths, &after.profile, verified_assets.root())?;
        if assets.generation_id() != verified_assets.generation_id()
            || profile.digest() != verified_profile.digest()
        {
            return Err(service_error(
                "renderer-runtime-content-changed",
                "the protected profile or runtime content changed while the renderer binding was verified",
            ));
        }

        let binding = RendererRuntimeBinding::new(
            *verified_assets.generation_id(),
            verified_profile.digest(),
        );
        let unity_font_hook = verified_profile.unity_font_hook_policy().clone();
        let private_freetype = verified_profile.private_freetype_policy();
        Ok(Self {
            assets: verified_assets,
            binding,
            unity_font_hook,
            private_freetype,
        })
    }

    pub const fn binding(&self) -> RendererRuntimeBinding {
        self.binding
    }

    pub const fn assets(&self) -> &ProtectedRuntimeAssets {
        &self.assets
    }

    pub const fn unity_font_hook_policy(&self) -> &UnityFontHookPolicy {
        &self.unity_font_hook
    }

    pub const fn private_freetype_policy(&self) -> PrivateFreeTypePolicy {
        self.private_freetype
    }
}

struct ActivePointerPair {
    profile_bytes: Vec<u8>,
    profile: GenerationPointer,
    runtime_bytes: Vec<u8>,
    runtime: RuntimeGenerationPointer,
}

impl ActivePointerPair {
    fn capture(paths: &MachinePaths) -> Result<Self, StructuredServiceError> {
        ensure_activation_state_is_stable(paths)?;
        let (profile_bytes, profile) = read_active_profile_pointer(paths)?;
        let (runtime_bytes, runtime) = read_active_runtime_pointer(paths)?;
        ensure_activation_state_is_stable(paths)?;
        Ok(Self {
            profile_bytes,
            profile,
            runtime_bytes,
            runtime,
        })
    }
}

fn service_error(code: &str, message: &str) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error: None,
    }
}
