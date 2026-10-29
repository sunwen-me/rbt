# Local static TF example

This example loads only the explicit TOML file passed with `--config`, validates
it, applies the static tree to a local software buffer, and maps a point from
`camera_optical` into `base_link` and back at one identified host monotonic
timestamp. It opens no hardware and publishes no transport endpoint.

The [software-only config](../../../docs/tf-config-example.toml) contains
illustrative values provided for this example. They are not measurements,
calibration output, or hardware extrinsics. The runtime prints the label
“Configured extrinsic — calibration accuracy unverified”.

Run from the repository root with the normal Rust toolchain:

```sh
cargo run -p rbt-tf-local -- --config docs/tf-config-example.toml
```

There is no default config path. The parser rejects files larger than 1 MiB,
unknown fields, invalid frames or rotations, duplicate child frames, cycles,
and transform counts above the requested bound before returning a config.

The example's host timestamp has an identified clock domain, while its actual
precision is unknown. It explicitly enables `allow_unknown_uncertainty`; output
continues to report `None` for timing uncertainty. This temporal uncertainty is
separate from the spatial calibration accuracy, which remains unverified.

The static set becomes valid at the supplied `effective_from` timestamp. TF
refuses lookups before that boundary, so a newly loaded calibration cannot be
applied to older samples. Replacing a static revision also replaces the previous
set; the buffer does not keep unbounded calibration history.
