# Documentation

| Audience | Entry point | Content |
| --- | --- | --- |
| Graph and node writers | [User documentation](user/index.md) | Guides, task pages, API and library reference |
| Engine contributors | [Development documentation](development/index.md) | Design, implementation, testing, decisions, proposals |

Both trees use the shared [terminology dictionary](terminology.md).

## Build and preview

The site requires Python 3.13 or later. It publishes only `docs/user/`.

```sh
python -m pip install -r docs/requirements.txt
python -m sphinx -E -a -b html --fail-on-warning --keep-going docs/user build/docs/html
python -m http.server 8000 --directory build/docs/html
```

Open <http://localhost:8000>. `.readthedocs.yaml` uses the same source tree and
dependencies.

## Maintenance

[Documentation maintenance](development/documentation.md) defines authoring,
example checks, and publication verification. `docs/tools/` contains the example
checker, download generator, and their tests.
