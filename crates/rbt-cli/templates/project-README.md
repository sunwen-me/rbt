# {{project_name}}

This project was created by `rbt new`. It is a Cargo workspace containing
activation-owned RBT components and an external generated-types crate.

## Run the example graph

Build and run the complete local graph with the canonical workflow:

```text
rbt check
rbt run
```

The source publishes a one-pixel `rbt::types::Image` through the `Hello`
capability and the sink prints its sequence/value. `Image` is used so the first
graph also works with the built-in `rbt record`/`rbt play` workflow. Both
components use the normal prepare/activate lifecycle and critical,
cancellation-aware tasks.

`types/` is generated from the `.rbt` files under the configured IDL source
directory. Run `rbt check` after editing schemas and `rbt idl generate` to
refresh bindings.
