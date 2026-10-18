//! Cryptographic building blocks.
//!
//! The skeleton fixes the *shapes* (algorithm ids, key lengths, envelope layout,
//! label separation) because those are the parts that are expensive to change
//! later; the primitive bodies carry `TODO(<crate>)`.

pub mod aead;
pub mod kdf;
pub mod sig;
