//! Hybrid logical clock.
//!
//! Plain `updatedAt` seconds (what `MergeDataDb` compares) has three failure
//! modes that show up as data loss:
//! * two writes in the same second compare equal and `>=` makes the *remote* one
//!   win, so a local edit is lost (H7);
//! * a VPS with a fast clock writes "the future", and every later honest write
//!   from another device loses until wall-clock time catches up;
//! * a VPS with a slow clock makes a device's newest write lose to an older one.
//!
//! A hybrid logical clock keeps wall-clock meaning (so timestamps stay roughly
//! comparable to `unix_ms` for retention and UI) while being monotone: the
//! logical part absorbs both regressions and same-millisecond writes, and
//! [`Hlc::observe`] makes a node advance past anything it has seen.

use core::cmp::Ordering;

/// `physical_ms` from the system clock, `logical` for ties and for absorbing a
/// clock that went backwards. Derived `Ord` compares `physical_ms` first, which
/// is exactly the intended ordering.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Hlc {
    pub physical_ms: u64,
    pub logical: u16,
}

impl Default for Hlc {
    /// No stamp yet. Same as [`Hlc::ZERO`], spelled as a trait so a struct that
    /// contains a clock can derive `Default` without inventing a second zero.
    fn default() -> Self {
        Self::ZERO
    }
}

/// Maximum logical steps inside one millisecond before we refuse. Refusing beats
/// silently borrowing from the future.
pub const MAX_LOGICAL: u16 = 65_535;

impl Hlc {
    pub const ZERO: Self = Self {
        physical_ms: 0,
        logical: 0,
    };

    pub fn new(physical_ms: u64, logical: u16) -> Self {
        Self {
            physical_ms,
            logical,
        }
    }

    pub fn is_zero(self) -> bool {
        self.physical_ms == 0 && self.logical == 0
    }

    /// Advance for a local write at wall-clock `now_ms`.
    ///
    /// * If the clock moved forward: take `now_ms`, logical 0.
    /// * If `now_ms` did not move (or went backwards): keep our physical time and
    ///   bump the logical counter, so the write is still strictly greater than
    ///   everything we have produced.
    pub fn tick(self, now_ms: u64) -> Hlc {
        if now_ms > self.physical_ms {
            Hlc::new(now_ms, 0)
        } else {
            Hlc::new(self.physical_ms, self.logical.saturating_add(1))
        }
    }

    /// Absorb a remote clock and return the value to stamp a *new* local write
    /// with. This is the step that makes "a device with a fast clock" harmless:
    /// we move our physical part up to theirs, so our next write is after theirs.
    pub fn observe(self, remote: Hlc, now_ms: u64) -> Hlc {
        let max_physical = self.physical_ms.max(remote.physical_ms).max(now_ms);
        if max_physical == self.physical_ms && max_physical == remote.physical_ms {
            Hlc::new(
                max_physical,
                self.logical.max(remote.logical).saturating_add(1),
            )
        } else if max_physical == self.physical_ms {
            Hlc::new(max_physical, self.logical.saturating_add(1))
        } else if max_physical == remote.physical_ms {
            Hlc::new(max_physical, remote.logical.saturating_add(1))
        } else {
            // `now_ms` is the largest, so the clocks are behind: start fresh.
            Hlc::new(max_physical, 0)
        }
    }

    /// Age in milliseconds relative to `now_ms`. Used by retention and by the
    /// "clock is implausibly far ahead" warning.
    pub fn age_ms(self, now_ms: u64) -> u64 {
        now_ms.saturating_sub(self.physical_ms)
    }

    /// True when a peer's clock is more than `tolerance_ms` ahead of ours. The
    /// agent logs this and *still accepts* the write: refusing would let a bad
    /// clock wedge a tenant.
    pub fn is_implausibly_ahead(self, now_ms: u64, tolerance_ms: u64) -> bool {
        self.physical_ms > now_ms.saturating_add(tolerance_ms)
    }

    /// Fixed 10-byte encoding for the SQLite key and the wire: 8 bytes BE
    /// physical + 2 bytes BE logical. Big-endian keeps byte order == clock order,
    /// so an index on the blob is an index on time.
    pub fn encode(self) -> [u8; 10] {
        let mut out = [0u8; 10];
        out[..8].copy_from_slice(&self.physical_ms.to_be_bytes());
        out[8..].copy_from_slice(&self.logical.to_be_bytes());
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != 10 {
            return None;
        }
        let mut physical = [0u8; 8];
        physical.copy_from_slice(&bytes[..8]);
        let mut logical_bytes = [0u8; 2];
        logical_bytes.copy_from_slice(&bytes[8..]);
        Some(Hlc::new(
            u64::from_be_bytes(physical),
            u16::from_be_bytes(logical_bytes),
        ))
    }

    /// `cmp` that is total and stable, spelled out so nobody "optimises" the
    /// logical tiebreak away.
    pub fn compare(self, other: Hlc) -> Ordering {
        match self.physical_ms.cmp(&other.physical_ms) {
            Ordering::Equal => self.logical.cmp(&other.logical),
            ordering => ordering,
        }
    }

    /// Wire form: `"<physical:016x>-<logical:04x>"`, e.g.
    /// `"0000018f3c1a2b40-0000"`.
    ///
    /// Hex rather than a JSON object because the value is opaque to the client —
    /// it echoes what it received — and one 22-byte string is cheaper to log and to
    /// compare than three fields. The width is fixed, so string order == clock
    /// order, which is a property tests can rely on.
    pub fn to_wire(self) -> String {
        format!("{:016x}-{:04x}", self.physical_ms, self.logical)
    }

    /// Parse [`Self::to_wire`]. Strict: exactly 16 hex digits, a dash, 4 hex digits.
    pub fn parse_wire(text: &str) -> Option<Self> {
        let (physical, logical) = text.split_once('-')?;
        if physical.len() != 16 || logical.len() != 4 {
            return None;
        }
        if !physical.bytes().all(|byte| byte.is_ascii_hexdigit())
            || !logical.bytes().all(|byte| byte.is_ascii_hexdigit())
        {
            return None;
        }
        Some(Hlc::new(
            u64::from_str_radix(physical, 16).ok()?,
            u16::from_str_radix(logical, 16).ok()?,
        ))
    }
}

/// Wall clock in milliseconds. Isolated so tests can inject a fake clock and so
/// there is exactly one place that reads the system time for ordering.
pub trait Clock {
    fn now_ms(&self) -> u64;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct SystemClock;

impl Clock for SystemClock {
    fn now_ms(&self) -> u64 {
        match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
            Ok(delta) => delta.as_millis() as u64,
            // Before 1970 is not a real host state; never panic on it.
            Err(_) => 0,
        }
    }
}

/// Monotone counter that survives a clock going backwards mid-run.
#[derive(Debug)]
pub struct HlcState {
    last: Hlc,
}

impl HlcState {
    pub fn new(last: Hlc) -> Self {
        Self { last }
    }

    pub fn last(&self) -> Hlc {
        self.last
    }

    /// Stamp a local write.
    pub fn tick(&mut self, clock: &dyn Clock) -> Hlc {
        self.last = self.last.tick(clock.now_ms());
        self.last
    }

    /// Absorb a remote clock seen in an incoming operation.
    pub fn observe(&mut self, remote: Hlc, clock: &dyn Clock) {
        self.last = self.last.observe(remote, clock.now_ms());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FakeClock(u64);
    impl Clock for FakeClock {
        fn now_ms(&self) -> u64 {
            self.0
        }
    }

    #[test]
    fn the_wire_form_roundtrips_and_sorts_like_the_clock() {
        for (physical, logical) in [(0u64, 0u16), (1_000, 0), (1_000, 7), (u64::MAX, 0xffff)] {
            let hlc = Hlc::new(physical, logical);
            let wire = hlc.to_wire();
            assert_eq!(wire.len(), 16 + 1 + 4);
            assert_eq!(Hlc::parse_wire(&wire), Some(hlc));
        }
        // Fixed width means string order == clock order, which lets a log or a test
        // compare wire strings.
        let early = Hlc::new(999, 0xffff).to_wire();
        let late = Hlc::new(1_000, 0).to_wire();
        assert!(early < late);
        assert_eq!(
            Hlc::new(0x018f_3c1a_2b40, 0).to_wire(),
            "0000018f3c1a2b40-0000"
        );
    }

    #[test]
    fn the_wire_parser_is_strict() {
        assert_eq!(
            Hlc::parse_wire("00000000000003e8-0000"),
            Some(Hlc::new(1_000, 0))
        );
        for bad in [
            "",
            "0003e8-0000",
            "00000000000003e8",
            "00000000000003e8-000",
            "00000000000003e8-00000",
            "zzzzzzzzzzzzzzzz-0000",
            "00000000000003e8-zzzz",
            "00000000000003e8_0000",
            " 00000000000003e8-0000",
        ] {
            assert_eq!(Hlc::parse_wire(bad), None, "{bad:?} must not parse");
        }
    }

    #[test]
    fn tick_is_monotone_when_clock_stalls() {
        let clock = FakeClock(1_000);
        let mut clock_state = HlcState::new(Hlc::ZERO);
        let a = clock_state.tick(&clock);
        let b = clock_state.tick(&clock);
        let c = clock_state.tick(&clock);
        assert!(a < b);
        assert!(b < c);
        assert_eq!(a.physical_ms, 1_000);
        assert_eq!(c.logical, 2);
    }

    #[test]
    fn tick_is_monotone_when_clock_goes_backwards() {
        let forward = FakeClock(5_000);
        let mut clock_state = HlcState::new(Hlc::ZERO);
        let a = clock_state.tick(&forward);
        let backward = FakeClock(1_000);
        let b = clock_state.tick(&backward);
        assert!(
            b > a,
            "a backwards wall clock must not produce an older stamp"
        );
        assert_eq!(b.physical_ms, 5_000);
        assert_eq!(b.logical, 1);
    }

    #[test]
    fn observe_absorbs_a_fast_peer() {
        let clock = FakeClock(1_000);
        let mut clock_state = HlcState::new(Hlc::ZERO);
        let _ = clock_state.tick(&clock);
        clock_state.observe(Hlc::new(9_000, 4), &clock);
        let next = clock_state.tick(&clock);
        assert!(next > Hlc::new(9_000, 4));
    }

    #[test]
    fn observe_absorbs_a_slow_peer() {
        let clock = FakeClock(9_000);
        let mut clock_state = HlcState::new(Hlc::ZERO);
        let _ = clock_state.tick(&clock);
        clock_state.observe(Hlc::new(10, 0), &clock);
        let next = clock_state.tick(&clock);
        assert!(next > Hlc::new(9_000, 0));
    }

    #[test]
    fn encoding_roundtrip_and_order() {
        let a = Hlc::new(u64::MAX - 1, 7);
        let encoded = a.encode();
        assert_eq!(Hlc::decode(&encoded), Some(a));
        assert_eq!(a.encode(), Hlc::decode(&a.encode()).unwrap().encode());
        // Byte order must match clock order.
        let smaller = Hlc::new(1, 0).encode();
        let larger = Hlc::new(2, 0).encode();
        assert!(smaller < larger);
    }

    #[test]
    fn implausible_clock_detected() {
        let stamp = Hlc::new(10_000_000, 0);
        assert!(stamp.is_implausibly_ahead(1_000, 60_000));
        assert!(!Hlc::new(2_000, 0).is_implausibly_ahead(1_000, 60_000));
    }
}
