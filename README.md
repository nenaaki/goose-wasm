# goose-wasm

A WebAssembly backend for the [Goose programming language](https://github.com/aardappel/goose).

This is an independent, unofficial project. It is not affiliated with or endorsed
by the Goose project or its author.

## Upstream

This project follows the reference Goose implementation:

- Repository: <https://github.com/aardappel/goose>
- Author: Wouter van Oortmerssen
- License: Apache License 2.0
- Included as a git submodule at [`upstream/goose`](upstream/goose), pinned to a
  specific commit. Its contents are unmodified and remain under their own licenses:
  see [`upstream/goose/LICENSE`](upstream/goose/LICENSE) and the license files in
  `upstream/goose/third_party/`.

The language semantics are those of the upstream specification
([`docs/goose_spec.md`](upstream/goose/docs/goose_spec.md)) and compiler. Where
this backend differs, the difference is a bug unless documented.

Clone with the submodule:

```sh
git clone --recurse-submodules https://github.com/nenaaki/goose-wasm.git
```

## License

Licensed under the Apache License, Version 2.0, the same license as upstream
Goose. See [LICENSE](LICENSE).

## Third-party notices

See [NOTICE](NOTICE).
