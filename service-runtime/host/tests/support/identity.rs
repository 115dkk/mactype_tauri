#![allow(dead_code)]

use mactype_service_contract::{ProfileDigest, RendererRuntimeBinding, RuntimeGenerationId};
use mactype_service_host::{ProcessArchitecture, ProcessIdentity};

pub(crate) const PROFILE_DIGEST: &str =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
pub(crate) const RUNTIME_GENERATION: &str =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

pub(crate) fn binding() -> RendererRuntimeBinding {
    RendererRuntimeBinding::new(
        RuntimeGenerationId::parse(RUNTIME_GENERATION).unwrap(),
        ProfileDigest::parse(PROFILE_DIGEST).unwrap(),
    )
}

pub(crate) fn identity(pid: u32, creation_time: u64) -> ProcessIdentity {
    ProcessIdentity {
        pid,
        creation_time,
        session_id: 2,
        architecture: ProcessArchitecture::X64,
    }
}
