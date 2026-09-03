//! COM apartment lifetime.

use windows::Win32::System::Com::{
    CoInitializeEx, CoUninitialize, COINIT_APARTMENTTHREADED, COINIT_MULTITHREADED,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ComThreading {
    Apartment,
    Multi,
}

/// A COM initialization on the current thread, uninitialized on drop. The
/// value is `!Send` by construction (it holds a raw pointer marker), so it
/// cannot leave the thread it initialized.
#[derive(Debug)]
pub struct ComApartment {
    _thread_bound: std::marker::PhantomData<*const ()>,
}

impl ComApartment {
    /// Initializes COM with `threading`. The error is the raw `HRESULT`.
    pub fn initialize(threading: ComThreading) -> Result<Self, i32> {
        let model = match threading {
            ComThreading::Apartment => COINIT_APARTMENTTHREADED,
            ComThreading::Multi => COINIT_MULTITHREADED,
        };
        // SAFETY: no reserved pointer is passed; the call only affects the
        // current thread's COM state, which this value now owns.
        unsafe { CoInitializeEx(None, model) }
            .ok()
            .map_err(|error| error.code().0)?;
        Ok(Self {
            _thread_bound: std::marker::PhantomData,
        })
    }
}

impl Drop for ComApartment {
    fn drop(&mut self) {
        // SAFETY: this value is the matching initialization for the current
        // thread and is dropped there, so the balance stays exact.
        unsafe { CoUninitialize() };
    }
}

#[cfg(test)]
mod tests {
    use super::{ComApartment, ComThreading};

    #[test]
    fn an_apartment_initializes_and_uninitializes_on_a_fresh_thread() {
        std::thread::spawn(|| {
            let apartment = ComApartment::initialize(ComThreading::Apartment).unwrap();
            drop(apartment);
            let again = ComApartment::initialize(ComThreading::Multi).unwrap();
            drop(again);
        })
        .join()
        .unwrap();
    }
}
