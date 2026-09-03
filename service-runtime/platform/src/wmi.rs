//! WMI access for process creation events.
//!
//! Every COM call on the WMI interfaces is `unsafe` in the `windows` crate
//! because the callee may read caller memory; the wrappers here pass only
//! owned `BSTR`s, local out values, and interface pointers the crate keeps
//! alive, and hand back plain integers or owned objects.

use std::time::Duration;

use windows::core::{IUnknown, Interface, BSTR};
use windows::Win32::Foundation::RPC_E_TOO_LATE;
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeSecurity, CoSetProxyBlanket, CLSCTX_INPROC_SERVER, EOAC_NONE,
    RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
};
use windows::Win32::System::Rpc::{RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE};
use windows::Win32::System::Variant::VARIANT;
use windows::Win32::System::Wmi::{
    IEnumWbemClassObject, IWbemClassObject, IWbemLocator, IWbemServices, WbemLocator,
    WBEM_FLAG_FORWARD_ONLY, WBEM_FLAG_RETURN_IMMEDIATELY, WBEM_S_TIMEDOUT,
};

/// A COM failure, reduced to its `HRESULT`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct WmiError {
    pub hresult: i32,
}

impl From<windows::core::Error> for WmiError {
    fn from(error: windows::core::Error) -> Self {
        Self {
            hresult: error.code().0,
        }
    }
}

/// A connection to `ROOT\CIMV2` with the proxy blanket the service needs.
/// Requires a multithreaded COM apartment on the calling thread.
pub struct WmiConnection {
    services: IWbemServices,
}

impl WmiConnection {
    /// Initializes process-wide COM security for the service identity. A
    /// second initialization (`RPC_E_TOO_LATE`) is accepted as already done.
    pub fn initialize_security() -> Result<(), WmiError> {
        // SAFETY: every optional argument is `None`; the integers select the
        // call-level authentication and impersonation the service needs.
        match unsafe {
            CoInitializeSecurity(
                None,
                -1,
                None,
                None,
                RPC_C_AUTHN_LEVEL_CALL,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                None,
                EOAC_NONE,
                None,
            )
        } {
            Ok(()) => Ok(()),
            Err(error) if error.code() == RPC_E_TOO_LATE => Ok(()),
            Err(error) => Err(error.into()),
        }
    }

    pub fn connect_cimv2() -> Result<Self, WmiError> {
        // SAFETY: the class and interface identifiers are WMI's own
        // constants; no outer unknown is passed.
        let locator: IWbemLocator =
            unsafe { CoCreateInstance(&WbemLocator, None::<&IUnknown>, CLSCTX_INPROC_SERVER) }?;
        let empty = BSTR::new();
        // SAFETY: every string is an owned BSTR that outlives the call and
        // the context is `None`.
        let services = unsafe {
            locator.ConnectServer(
                &BSTR::from("ROOT\\CIMV2"),
                &empty,
                &empty,
                &empty,
                0,
                &empty,
                None,
            )
        }?;
        // SAFETY: `services` is a live interface; every optional argument is
        // `None` and the integers select the documented blanket.
        unsafe {
            CoSetProxyBlanket(
                &services,
                RPC_C_AUTHN_WINNT,
                RPC_C_AUTHZ_NONE,
                None,
                RPC_C_AUTHN_LEVEL_CALL,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                None,
                EOAC_NONE,
            )
        }?;
        Ok(Self { services })
    }

    /// A forward-only event subscription for `wql`.
    pub fn notification_query(&self, wql: &str) -> Result<WmiEnumerator, WmiError> {
        let flags = WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY;
        // SAFETY: both strings are owned BSTRs that outlive the call; the
        // context is `None`.
        let enumerator = unsafe {
            self.services
                .ExecNotificationQuery(&BSTR::from("WQL"), &BSTR::from(wql), flags, None)
        }?;
        Ok(WmiEnumerator { enumerator })
    }

    /// A forward-only instance query for `wql`.
    pub fn query(&self, wql: &str) -> Result<WmiEnumerator, WmiError> {
        let flags = WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY;
        // SAFETY: both strings are owned BSTRs that outlive the call; the
        // context is `None`.
        let enumerator = unsafe {
            self.services
                .ExecQuery(&BSTR::from("WQL"), &BSTR::from(wql), flags, None)
        }?;
        Ok(WmiEnumerator { enumerator })
    }
}

/// An enumerator over query results or events.
pub struct WmiEnumerator {
    enumerator: IEnumWbemClassObject,
}

impl WmiEnumerator {
    /// The next object, or `None` when the enumeration is exhausted or
    /// `timeout` passed without an event.
    pub fn next(&self, timeout: Duration) -> Result<Option<WmiObject>, WmiError> {
        let timeout_ms = timeout.as_millis().min(i32::MAX as u128) as i32;
        let mut objects = [None];
        let mut returned = 0;
        // SAFETY: `objects` and `returned` are locals the binding fills; the
        // enumerator is a live interface.
        let result = unsafe {
            self.enumerator
                .Next(timeout_ms, &mut objects, &mut returned)
        };
        if result.0 == WBEM_S_TIMEDOUT.0 || returned == 0 {
            return Ok(None);
        }
        result.ok()?;
        Ok(objects[0].take().map(|object| WmiObject { object }))
    }
}

/// One WMI object.
pub struct WmiObject {
    object: IWbemClassObject,
}

impl WmiObject {
    /// The `ProcessID` property, when present and numeric.
    pub fn process_id(&self) -> Result<u32, WmiError> {
        let mut value = VARIANT::default();
        // SAFETY: the property name is a static wide string and `value` is a
        // local out variant; the object is a live interface.
        unsafe {
            self.object
                .Get(windows::core::w!("ProcessID"), 0, &mut value, None, None)
        }?;
        u32::try_from(&value).map_err(Into::into)
    }

    /// The `TargetInstance` of an event, as an object.
    pub fn target_instance(&self) -> Result<WmiObject, WmiError> {
        let mut value = VARIANT::default();
        // SAFETY: the property name is a static wide string and `value` is a
        // local out variant; the object is a live interface.
        unsafe {
            self.object.Get(
                windows::core::w!("TargetInstance"),
                0,
                &mut value,
                None,
                None,
            )
        }?;
        let unknown = IUnknown::try_from(&value)?;
        let object: IWbemClassObject = unknown.cast()?;
        Ok(WmiObject { object })
    }
}
