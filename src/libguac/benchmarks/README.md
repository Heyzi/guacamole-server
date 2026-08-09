# libguac benchmarks

Benchmarks are optional and are not built by `make` or run by `make check`.
After configuring the source tree, build and run a benchmark explicitly:

```sh
make -C src/libguac benchmark-display-flush-copy
src/libguac/benchmark-display-flush-copy
```

The display-flush-copy benchmark uses 15 samples by default. Override this
with `--runs N`; at least five samples are required:

```sh
src/libguac/benchmark-display-flush-copy --runs 25
```
