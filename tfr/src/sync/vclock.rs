//! Vector clocks for causality, and the reason they are needed.
//!
//! `MergeDataDb` decides conflicts with `WHERE r.updatedAt >= l.updatedAt`. That
//! is a *timestamp*, and a timestamp cannot distinguish:
//!
//! * "device B saw A's edit and then made its own" — B must win, and
//! * "device B edited at the same time without seeing A's edit" — nothing may win
//!   silently.
//!
//! Both look like "B is newer". A vector clock makes the difference explicit:
//! `B` dominates `A` in the first case (it has A's counter) and is *concurrent*
//! in the second. The engine uses that to merge disjoint fields and to surface a
//! real conflict for the same field.

use std::collections::BTreeMap;

/// Device id → highest sequence that device has applied for this entity.
///
/// `BTreeMap` rather than `HashMap`: the encoding must be byte-stable for the
/// SQLite key, and iteration order must not depend on hashing.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VectorClock {
    entries: BTreeMap<String, u64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CausalOrder {
    /// `self` happened strictly before `other`.
    Before,
    /// `self` happened strictly after `other`.
    After,
    /// Identical counters: the same set of writes.
    Equal,
    /// Neither dominates: a genuine conflict.
    Concurrent,
}

impl VectorClock {
    pub fn new() -> Self {
        Self::default()
    }

    /// Build from pairs. Duplicate device ids keep the maximum.
    pub fn from_pairs(pairs: &[(&str, u64)]) -> Self {
        let mut clock = Self::default();
        for (device, seq) in pairs {
            clock.merge_entry(device, *seq);
        }
        clock
    }

    pub fn get(&self, device: &str) -> u64 {
        self.entries.get(device).copied().unwrap_or(0)
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn devices(&self) -> impl Iterator<Item = (&String, &u64)> {
        self.entries.iter()
    }

    /// Record that `device` produced its next write.
    pub fn increment(&mut self, device: &str, seq: u64) {
        self.merge_entry(device, seq);
    }

    fn merge_entry(&mut self, device: &str, seq: u64) {
        let slot = self.entries.entry(device.to_string()).or_insert(0);
        if seq > *slot {
            *slot = seq;
        }
    }

    /// Pointwise maximum. Used after applying a remote write: the local clock must
    /// then dominate both, otherwise a later read would classify our own state as
    /// concurrent.
    pub fn merge(&self, other: &Self) -> Self {
        let mut out = self.clone();
        for (device, seq) in other.entries.iter() {
            out.merge_entry(device, *seq);
        }
        out
    }

    /// Domination test.
    pub fn compare(&self, other: &Self) -> CausalOrder {
        let mut self_greater = false;
        let mut other_greater = false;
        for (device, seq) in self.entries.iter() {
            match seq.cmp(&other.get(device)) {
                std::cmp::Ordering::Greater => self_greater = true,
                std::cmp::Ordering::Less => other_greater = true,
                std::cmp::Ordering::Equal => {}
            }
        }
        for (device, seq) in other.entries.iter() {
            if *seq > self.get(device) {
                other_greater = true;
            }
        }
        match (self_greater, other_greater) {
            (false, false) => CausalOrder::Equal,
            (true, false) => CausalOrder::After,
            (false, true) => CausalOrder::Before,
            (true, true) => CausalOrder::Concurrent,
        }
    }

    /// Total order used only for a *deterministic tiebreak* when a conflict must
    /// be presented to the user in a stable order. Never used to pick a winner.
    pub fn deterministic_tiebreak(&self) -> Vec<(String, u64)> {
        self.entries
            .iter()
            .map(|(device, seq)| (device.clone(), *seq))
            .collect()
    }

    /// `device:seq,device:seq` — compact, sortable, and safe in a SQLite text
    /// column. Chosen over a binary encoding because it is greppable while
    /// debugging, and this metadata is small.
    pub fn encode(&self) -> String {
        let mut parts: Vec<String> = Vec::with_capacity(self.entries.len());
        for (device, seq) in self.entries.iter() {
            parts.push(format!("{device}:{seq}"));
        }
        parts.join(",")
    }

    pub fn decode(text: &str) -> Result<Self, String> {
        let mut clock = Self::default();
        for part in text.split(',').filter(|p| !p.is_empty()) {
            let (device, seq_text) = part
                .rsplit_once(':')
                .ok_or_else(|| format!("bad vector clock element {part:?}"))?;
            let seq: u64 = seq_text
                .parse()
                .map_err(|_| format!("bad sequence {seq_text:?}"))?;
            if device.is_empty() {
                return Err("empty device id in vector clock".to_string());
            }
            clock.merge_entry(device, seq);
        }
        Ok(clock)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn concurrent_edits_are_detected() {
        let a = VectorClock::from_pairs(&[("dev-a", 3)]);
        let b = VectorClock::from_pairs(&[("dev-b", 1)]);
        assert_eq!(a.compare(&b), CausalOrder::Concurrent);
        assert_eq!(b.compare(&a), CausalOrder::Concurrent);
    }

    #[test]
    fn causality_is_detected() {
        let a = VectorClock::from_pairs(&[("dev-a", 3)]);
        let b = VectorClock::from_pairs(&[("dev-a", 3), ("dev-b", 1)]);
        assert_eq!(a.compare(&b), CausalOrder::Before);
        assert_eq!(b.compare(&a), CausalOrder::After);
    }

    #[test]
    fn equal_and_empty() {
        let a = VectorClock::new();
        let b = VectorClock::new();
        assert_eq!(a.compare(&b), CausalOrder::Equal);
        let c = VectorClock::from_pairs(&[("dev-a", 1)]);
        assert_eq!(c.compare(&c.clone()), CausalOrder::Equal);
    }

    #[test]
    fn merge_dominates_both() {
        let a = VectorClock::from_pairs(&[("dev-a", 5)]);
        let b = VectorClock::from_pairs(&[("dev-b", 2)]);
        let merged = a.merge(&b);
        assert_eq!(merged.compare(&a), CausalOrder::After);
        assert_eq!(merged.compare(&b), CausalOrder::After);
    }

    #[test]
    fn encode_decode_roundtrip() {
        let clock = VectorClock::from_pairs(&[("dev-a", 1), ("dev-b", 42)]);
        let encoded = clock.encode();
        assert_eq!(VectorClock::decode(&encoded).unwrap(), clock);
        assert!(VectorClock::decode("").unwrap().is_empty());
        assert!(VectorClock::decode("broken").is_err());
        assert!(VectorClock::decode("dev:x").is_err());
    }
}
