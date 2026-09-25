## Agent skills

### Issue tracker

Issues live as markdown files under `.scratch/<feature>/` in this repo. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, used verbatim as label strings. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

## Coding standards

RakNet uses **no exceptions**: nothing in `Source/` throws or catches, and library code
reports failure by return value. Standard-library APIs that throw for anything *other*
than allocation failure are off-limits there (`std::stoi`, throwing `std::filesystem`
overloads, `std::random_device`, ...). Allocation failure is treated as fatal, so
containers and `new` are fine. Allocation sizes read from the wire or a file must still be
bounded before allocating. Tests are exempt. See `docs/adr/0002-raknet-does-not-use-exceptions.md`
and `docs/adr/0003-allocation-failure-is-fatal.md`.
