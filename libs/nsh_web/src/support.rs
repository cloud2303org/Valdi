//! The small engine-independent support that replaces NSH UI image types.
use std::{ops::Deref, sync::Arc};
#[derive(Clone)]
pub(crate) struct SharedBytes(Arc<dyn AsRef<[u8]> + Send + Sync>);
impl SharedBytes {
    pub(crate) fn new(bytes: impl AsRef<[u8]> + Send + Sync + 'static) -> Self {
        Self(Arc::new(bytes))
    }
}
impl Deref for SharedBytes {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        self.0.as_ref().as_ref()
    }
}
pub(crate) fn unix_ms() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs_f64()
        * 1000.0
}
