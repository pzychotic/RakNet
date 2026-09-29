## Agent skills

### Issue tracker

Issues live as markdown files under `.scratch/<feature>/` in this repo. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, used verbatim as label strings. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

## Coding standards

RakNet is **exception-neutral**: `Source/` builds with exceptions disabled and fails the
same way with them enabled. Library code reports failure by return value and never throws
or catches. Allocation failure and `std::mutex::lock` failure are fatal, so containers and
`new` are fine. Every size or count a System drives, whether a length off the wire or
objects created per remote message, is bounded before allocating. APIs that throw for a
recoverable failure are off-limits: `std::stoi`, throwing `std::filesystem` overloads,
`std::random_device`, `.at()`, `std::string`'s position-taking members, and the
`std::thread` constructor. Tests are exempt. See `docs/adr/0004-raknet-is-exception-neutral.md`.

### Conventions

- In code comments, docs and commit messages, cite an ADR by number only ("ADR-0004"), not by file path.
- Code comments state the settled fact. The reasoning behind a change goes in the commit
  message, not the code. Don't reference tickets or `.scratch/`.
- New files don't carry the Oculus VR copyright header. It belongs only to files inherited from upstream.
