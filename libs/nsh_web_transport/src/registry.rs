//! Process-wide lookup only; callback serialization belongs to each request.
use crate::{EventSink, FetchEvent};
use std::{
    collections::HashMap,
    hash::Hash,
    sync::{Arc, Mutex},
};
pub(crate) struct EventRegistry<K>(Mutex<HashMap<K, Arc<EventSink>>>);
impl<K> Default for EventRegistry<K> {
    fn default() -> Self {
        Self(Mutex::new(HashMap::new()))
    }
}
impl<K: Eq + Hash> EventRegistry<K> {
    pub(crate) fn insert(&self, key: K, events: Arc<EventSink>) {
        self.0.lock().expect("requests").insert(key, events);
    }
    pub(crate) fn remove(&self, key: &K) {
        let events = self.0.lock().expect("requests").remove(key);
        if let Some(events) = events {
            events.clear();
        }
    }
    pub(crate) fn report(&self, key: &K, event: FetchEvent) {
        let terminal = matches!(event, FetchEvent::End | FetchEvent::Failed(_));
        let events = {
            let mut map = self.0.lock().expect("requests");
            if terminal {
                map.remove(key)
            } else {
                map.get(key).cloned()
            }
        };
        // A callback may wait for its context to drain. It must never retain
        // the process map lock needed by unrelated contexts and cancellation.
        if let Some(events) = events {
            events.send(event);
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::{sync::mpsc, time::Duration};
    #[test]
    fn blocked_request_callback_does_not_hold_process_registry() {
        let registry = Arc::new(EventRegistry::default());
        let (entered, seen) = mpsc::channel();
        let (resume, held) = mpsc::channel();
        registry.insert(
            1,
            Arc::new(EventSink::new(Box::new(move |_| {
                entered.send(()).unwrap();
                held.recv().unwrap();
            }))),
        );
        let first = registry.clone();
        let blocked = std::thread::spawn(move || first.report(&1, FetchEvent::BodyWanted));
        seen.recv_timeout(Duration::from_secs(2)).unwrap();
        let second = registry.clone();
        let (tx, rx) = mpsc::channel();
        let unrelated = std::thread::spawn(move || {
            second.insert(2, Arc::new(EventSink::new(Box::new(|_| {}))));
            second.report(&2, FetchEvent::End);
            second.remove(&2);
            let _ = tx.send(());
        });
        let progress = rx.recv_timeout(Duration::from_millis(300));
        resume.send(()).unwrap();
        blocked.join().unwrap();
        unrelated.join().unwrap();
        assert!(
            progress.is_ok(),
            "an unrelated request was blocked by a callback holding the process registry: {progress:?}"
        );
    }
}
