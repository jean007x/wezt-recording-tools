# Clean-Room Policy

Wezt Recording Tools replicates *categories of functionality* (multitrack recording, editing,
mixing, plugin hosting). It does not copy any specific product.

## Rules

1. **No proprietary code.** Contributors must not read, decompile, or paste code from
   Avid, Apple or any closed-source DAW, including leaked source.
2. **No proprietary assets or branding.** No product names, logos, icons, sounds,
   presets, factory content, UI strings or screen layouts copied from other products.
   Names like "Pro Tools", "Logic", "AAX" (as a product brand) appear only in
   descriptive compatibility statements.
3. **Specs over binaries.** Behavior is implemented from public specifications and
   standards: AES31/BWF, AAF (AMWA), OMF, MIDI 1.0/2.0, MusicXML, VST3 (open
   source licence), CLAP (MIT), AU (Apple developer documentation).
4. **Two-team separation for any proprietary format.** If a proprietary file format
   is ever supported, one contributor documents observable behavior from sample files
   the user lawfully owns; a different contributor implements from that written
   description only. Reverse engineering is only done where lawful in the
   contributor's jurisdiction. This project does **not** ship a `.ptx` reader.
5. **Contributor attestation.** Every PR states the author has followed this policy.

## What compatibility means here

Cross-version and cross-DAW compatibility is delivered through open interchange
formats (see `docs/ARCHITECTURE.md`, "Interchange"), and through Wezt Recording Tools's own
versioned session format with forward migrations.

This document is a project policy, not legal advice. Get a lawyer before shipping
anything that touches third-party SDKs or formats.
