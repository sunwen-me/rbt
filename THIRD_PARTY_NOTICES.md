# Third-Party Notices

This repository uses the maintained `iceoryx2 0.10.0-rbt.2` baseline for local
interprocess communication and its network integration. A historical patched
0.9.3 source tree remains for defect reproduction; its attribution follows.

- Upstream project: [eclipse-iceoryx/iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2/tree/v0.9.3)
- Crate metadata: [iceoryx2 0.9.3 on crates.io](https://crates.io/crates/iceoryx2/0.9.3)
- Upstream licensing: the `iceoryx2` 0.9.3 crate metadata declares
  `MIT OR Apache-2.0`. See the upstream
  [Apache-2.0 license](https://github.com/eclipse-iceoryx/iceoryx2/blob/v0.9.3/LICENSE-APACHE)
  and [MIT license](https://github.com/eclipse-iceoryx/iceoryx2/blob/v0.9.3/LICENSE-MIT).

The dependency graph also contains transitive dependencies. Each transitive
dependency retains its own license and attribution terms; this notice does not
relicense or replace them. The exact locked dependency set is recorded in
`Cargo.lock`; consult each dependency's package metadata and source
distribution for its applicable license and notices when redistributing.

The unified maintained production baseline uses upstream iceoryx2 `v0.10.0`,
commit `135d09dd8b29f321f1725920d434864c4e512378`, with the patches and build
provenance in `third_party/iceoryx2-rbt`. It retains upstream's
`MIT OR Apache-2.0` licensing and copyright notices. Its Zenoh integration also
retains the licensing and notices of the upstream source and locked
dependencies. The full source checkout includes the upstream `NOTICE.md`,
`LICENSE-MIT`, and `LICENSE-APACHE`; redistribute those with the maintained
source. Rust, C++, Python and network builds use this same maintained patch set.

The native `rbt` record/info/play tooling uses the Rust crate `mcap` version
`0.25.0`, pinned without default compression features, to read and write the
MCAP container format.

- Upstream project: [foxglove/mcap](https://github.com/foxglove/mcap)
- Format website: [mcap.dev](https://mcap.dev/)
- Crate metadata: [mcap 0.25.0 on crates.io](https://crates.io/crates/mcap/0.25.0)
- Upstream licensing: the crate metadata declares `MIT`; see the upstream
  [license](https://github.com/foxglove/mcap/blob/main/LICENSE).

The repository's project license texts are [LICENSE-APACHE](LICENSE-APACHE) and
[LICENSE-MIT](LICENSE-MIT). This file is an attribution index, not a substitute
for the upstream license texts.
